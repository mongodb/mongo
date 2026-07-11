// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoRandCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoRandCheck::MongoRandCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoRandCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // Matcher for srand and sand functions
    Finder->addMatcher(
        callExpr(callee(functionDecl(hasAnyName("::srand", "::rand")))).bind("callExpr"), this);
}

void MongoRandCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    // Get the matched check
    const auto* CallExpr = Result.Nodes.getNodeAs<clang::CallExpr>("callExpr");
    if (CallExpr) {
        diag(CallExpr->getBeginLoc(),
             "Use of rand or srand, use <random> or PseudoRandom instead.");
    }
}
}  // namespace mongo::tidy
