// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoFCVConstantCheck.h"

#include <clang/AST/ComputeDependence.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoFCVConstantCheck::MongoFCVConstantCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoFCVConstantCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {

    Finder->addMatcher(
        // Match a FCV comparison function whose argument is a FCV constant.
        declRefExpr(
            // Find a reference to FeatureCompatibilityVersion enum.
            hasDeclaration(enumConstantDecl(
                hasType(enumDecl(hasName("mongo::multiversion::FeatureCompatibilityVersion"))))),
            // Find a call to FCV comparison functions.
            hasParent(callExpr(
                anyOf(callee(functionDecl(hasName("FCVSnapshot::isLessThan"))),
                      callee(functionDecl(hasName("FCVSnapshot::isGreaterThan"))),
                      callee(functionDecl(hasName("FCVSnapshot::isLessThanOrEqualTo"))),
                      callee(functionDecl(hasName("FCVSnapshot::isGreaterThanOrEqualTo"))),
                      callee(functionDecl(hasName("FCVSnapshot::isUpgradingOrDowngrading")))))))
            .bind("fcv_constant"),
        this);
}

void MongoFCVConstantCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    const auto* loc_match = Result.Nodes.getNodeAs<DeclRefExpr>("fcv_constant");
    if (loc_match) {
        diag(loc_match->getBeginLoc(),
             "Illegal use of FCV constant in FCV comparison check functions. FCV gating should be "
             "done through feature flags instead.");
    }
}

}  // namespace mongo::tidy
