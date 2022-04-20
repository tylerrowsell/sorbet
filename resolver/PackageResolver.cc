#include "resolver/PackageResolver.h"
#include "ast/treemap/treemap.h"
#include "common/formatting.h"
#include "core/Context.h"
#include "core/packages/PackageDB.h"
#include "core/Unfreeze.h"

namespace sorbet::resolver {

namespace {

struct Import {
    core::NameRef pkg;

    // TODO: inlined vector
    std::vector<core::NameRef> prefix;

    static Import build(const core::GlobalState &gs, core::NameRef pkg) {
        Import res;
        auto &info = gs.packageDB().getPackageInfo(pkg);
        res.pkg = pkg;
        res.prefix = info.fullName();
        return res;
    }
};

// This could be built once per package and shared between all of the files
struct ImportEnv final {
    const core::packages::ImportInfo imports;

    std::vector<Import> regularImports;

    ImportEnv(core::packages::ImportInfo imports) : imports{std::move(imports)} {}

    static ImportEnv build(const core::GlobalState &gs, const core::packages::PackageInfo &info) {
        auto imports = core::packages::ImportInfo::fromPackage(gs, info);
        ImportEnv res{std::move(imports)};

        for (auto pkg : res.imports.regularImports) {
            res.regularImports.emplace_back(Import::build(gs, pkg));
        }

        return res;
    }

    const Import* isRegularImport(const std::vector<core::NameRef> &scope) const {
        for (auto &import : this->regularImports) {
            if (import.prefix == scope) {
                return &import;
            }
        }

        return nullptr;
    }
};

struct PackageResolverPass final {
    const ImportEnv imports;

    // Very similar to the nesting in resolver.cc, but without shared pointers as we make the decision about resolving
    // at the point we encounter the constant, and don't consider deferring it to later.
    std::vector<core::SymbolRef> nesting;

    struct NameWithScope {
        ast::ExpressionPtr *root = nullptr;
        std::vector<core::NameRef> scope;
        core::NameRef name;
    };

    NameWithScope collectScope(const core::GlobalState &gs, ast::UnresolvedConstantLit *lit) {
        NameWithScope res;

        res.name = lit->cnst;
        lit = ast::cast_tree<ast::UnresolvedConstantLit>(lit->scope);

        while (lit != nullptr && !lit->cnst.isPackagerName(gs)) {
            res.scope.emplace_back(lit->cnst);
            auto *next = ast::cast_tree<ast::UnresolvedConstantLit>(lit->scope);

            if (next == nullptr) {
                res.root = &lit->scope;
            }

            lit = next;
        }

        absl::c_reverse(res.scope);

        return res;
    }

    ast::ExpressionPtr postTransformUnresolvedConstantLit(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto scope = collectScope(ctx, ast::cast_tree<ast::UnresolvedConstantLit>(tree));
        if (scope.root == nullptr) {
            return tree;
        }

        // handle the case where the constant might refer to something in the current package's namespace (parent or
        // child package prefix)
        // TODO

        // check if the constant matches a top-level import
        if (auto *import = this->imports.isRegularImport(scope.scope)) {

            // TODO: no
            fmt::print("{} {}\n", import->pkg.showRaw(ctx), import->pkg.kind());
            auto uniqueName = import->pkg.dataCnst(ctx)->original;
            auto utf8Name = uniqueName.dataUnique(ctx)->original;
            auto pkgPriv = ctx.state.lookupNameUnique(core::UniqueNameKind::PackagerPrivate, utf8Name, 1);
            if (!pkgPriv.exists()) {
                pkgPriv = ctx.state.freshNameUnique(core::UniqueNameKind::PackagerPrivate, utf8Name, 1);
                pkgPriv = ctx.state.enterNameConstant(pkgPriv);
            }

            auto loc = scope.root->loc();
            auto reg = ast::make_expression<ast::UnresolvedConstantLit>(loc, std::move(*scope.root), core::Names::Constants::PackageRegistry());
            *scope.root = ast::make_expression<ast::UnresolvedConstantLit>(loc, std::move(reg), pkgPriv);
        }

        return tree;
    }

    ast::ExpressionPtr preTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &klass = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        auto *name = ast::cast_tree<ast::ConstantLit>(klass.name);
        if (name == nullptr) {
            ENFORCE(klass.symbol == core::Symbols::root());
            return tree;
        }

        this->nesting.emplace_back(klass.symbol);
        return tree;
    }

    ast::ExpressionPtr postTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &klass = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        auto *name = ast::cast_tree<ast::ConstantLit>(klass.name);
        if (name == nullptr) {
            ENFORCE(klass.symbol == core::Symbols::root());
            return tree;
        }

        this->nesting.pop_back();
        return tree;
    }

    PackageResolverPass(ImportEnv imports) : imports{std::move(imports)} {}
};

} // namespace

ast::ParsedFilesOrCancelled PackageResolver::run(core::GlobalState &gs, std::vector<ast::ParsedFile> trees,
                                                 WorkerPool &workers) {
    auto &packageDB = gs.packageDB();
    core::UnfreezeNameTable unfreeze(gs);

    // TODO: parallelize over trees using workers
    for (auto &tree : trees) {
        fmt::print("# {}\n", tree.file.data(gs).path());

        auto imports = ImportEnv::build(gs, packageDB.getPackageForFile(gs, tree.file));

        PackageResolverPass pass{std::move(imports)};
        core::MutableContext ctx{gs, core::Symbols::root(), tree.file};

        for (auto import : pass.imports.regularImports) {
            fmt::print("imports: {}\n",
                       fmt::map_join(import.prefix, "::", [&ctx](auto name) { return name.show(ctx); }));
        }

        tree.tree = ast::TreeMap::apply(ctx, pass, std::move(tree.tree));
    }

    return trees;
}

} // namespace sorbet::resolver
