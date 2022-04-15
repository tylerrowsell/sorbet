#include "resolver/PackageResolver.h"
#include "ast/treemap/treemap.h"
#include "common/formatting.h"
#include "core/Context.h"
#include "core/packages/PackageDB.h"

namespace sorbet::resolver {

namespace {

struct PackageResolverPass final {
    const core::packages::PackageInfo &pkg;

    // Very similar to the nesting in resolver.cc, but without shared pointers as we make the decision about resolving
    // at the point we encounter the constant, and don't consider deferring it to later.
    std::vector<core::SymbolRef> nesting;

    bool resolvePackage(core::Context ctx, ast::ExpressionPtr &out, std::vector<core::NameRef> &scope,
                      ast::UnresolvedConstantLit &lit) {
        if (lit.cnst.isPackagerName(ctx)) {
            return false;
        }

        // ensure that we have the full scope present
        if (auto *s = ast::cast_tree<ast::UnresolvedConstantLit>(lit.scope)) {
            return resolvePackage(ctx, out, scope, *s);
        }

        scope.emplace_back(lit.cnst);

        fmt::print("scope: {}\n", fmt::map_join(scope, "::", [&ctx](auto name) { return name.show(ctx); }));

        return true;
    }

    ast::ExpressionPtr postTransformUnresolvedConstantLit(core::Context ctx, ast::ExpressionPtr tree) {
        auto &cnst = ast::cast_tree_nonnull<ast::UnresolvedConstantLit>(tree);

        ast::ExpressionPtr out;
        std::vector<core::NameRef> scope;

        resolvePackage(ctx, out, scope, cnst);

        for (auto parent : this->nesting) {
            fmt::print("parent: {}\n", parent.show(ctx));
        }

        return tree;
    }

    ast::ExpressionPtr preTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
        auto &klass = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        auto *name = ast::cast_tree<ast::ConstantLit>(klass.name);
        if (name == nullptr) {
            ENFORCE(klass.symbol == core::Symbols::root());
            return tree;
        }

        this->nesting.emplace_back(klass.symbol);
        return tree;
    }

    ast::ExpressionPtr postTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
        auto &klass = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        auto *name = ast::cast_tree<ast::ConstantLit>(klass.name);
        if (name == nullptr) {
            ENFORCE(klass.symbol == core::Symbols::root());
            return tree;
        }

        this->nesting.pop_back();
        return tree;
    }

    PackageResolverPass(const core::packages::PackageInfo &pkg) : pkg{pkg} {}
};

} // namespace

ast::ParsedFilesOrCancelled PackageResolver::run(core::GlobalState &gs, std::vector<ast::ParsedFile> trees,
                                                 WorkerPool &workers) {
    auto &packageDB = gs.packageDB();

    // TODO: parallelize over trees using workers
    for (auto &tree : trees) {
        fmt::print("# {}\n", tree.file.data(gs).path());
        PackageResolverPass pass{packageDB.getPackageForFile(gs, tree.file)};
        core::Context ctx{gs, core::Symbols::root(), tree.file};
        tree.tree = ast::TreeMap::apply(ctx, pass, std::move(tree.tree));
    }

    return trees;
}

} // namespace sorbet::resolver
