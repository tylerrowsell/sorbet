#ifndef RESOLVER_PACKAGE_RESOLVER_H
#define RESOLVER_PACKAGE_RESOLVER_H

#include <vector>

#include "ast/ast.h"

namespace sorbet::resolver {

class PackageResolver final {
    ~PackageResolver() = delete;

public:
    static ast::ParsedFilesOrCancelled run(core::GlobalState &gs, std::vector<ast::ParsedFile> trees,
                                           WorkerPool &workers);
};

} // namespace sorbet::resolver

#endif
