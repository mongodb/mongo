// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoUnstructuredLogCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoUnstructuredLogCheck::MongoUnstructuredLogCheck(StringRef Name,
                                                     clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoUnstructuredLogCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // match function calls to either 'logd' or 'doUnstructuredLogImpl' functions
    Finder->addMatcher(
        callExpr(callee(functionDecl(anyOf(hasName("logd"), hasName("doUnstructuredLogImpl")))))
            .bind("unstructuredLogCall"),
        this);
}

void MongoUnstructuredLogCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    // Get the matched unstructured logging call
    const auto* unstructuredLogCall = Result.Nodes.getNodeAs<CallExpr>("unstructuredLogCall");
    if (unstructuredLogCall) {
        diag(unstructuredLogCall->getBeginLoc(),
             "Illegal use of unstructured logging, this is only for local "
             "development use and should not be committed");
    }
}

}  // namespace mongo::tidy
