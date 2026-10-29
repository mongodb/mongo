// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoInvariantStatusIsOKCheck.h"

#include <iostream>

#include "MongoTidyUtils.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoInvariantStatusIsOKCheck::MongoInvariantStatusIsOKCheck(StringRef Name,
                                                             clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoInvariantStatusIsOKCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {

    Finder->addMatcher(
        callExpr(callee(functionDecl(anyOf(hasName("invariantWithContextAndLocation"),
                                           hasName("invariantWithLocation")))),
                 hasArgument(0,
                             cxxMemberCallExpr(on(hasType(cxxRecordDecl(anyOf(
                                                   hasName("Status"), hasName("StatusWith"))))),
                                               callee(cxxMethodDecl(hasName("isOK"))))))
            .bind("invariant_call"),
        this);
}

void MongoInvariantStatusIsOKCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    const auto* invariantCall = Result.Nodes.getNodeAs<CallExpr>("invariant_call");

    if (invariantCall) {
        diag(invariantCall->getBeginLoc(),
             "Found invariant(status.isOK()) or dassert(status.isOK()), use invariant(status) for "
             "better diagnostics");
    }
}

}  // namespace mongo::tidy
