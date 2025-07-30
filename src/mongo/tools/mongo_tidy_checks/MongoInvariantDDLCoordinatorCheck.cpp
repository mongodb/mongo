/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "MongoInvariantDDLCoordinatorCheck.h"

using namespace clang::ast_matchers;

namespace mongo::tidy {

InvariantDDLCoordinatorCheck::InvariantDDLCoordinatorCheck(clang::StringRef Name,
                                                           clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void InvariantDDLCoordinatorCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(cxxMethodDecl(ofClass(cxxRecordDecl(anyOf(
                                         isSameOrDerivedFrom(hasName("ShardingDDLCoordinator")),
                                         isSameOrDerivedFrom(hasName("ConfigsvrCoordinator"))))))
                           .bind("ddl_coordinator_method"),
                       this);

    // Note: invariant() is a macro, translated by the preprocessor to these functions.
    Finder->addMatcher(
        callExpr(callee(functionDecl(anyOf(hasName("invariantWithContextAndLocation"),
                                           hasName("invariantWithLocation")))))
            .bind("invariant_call"),
        this);
}

void InvariantDDLCoordinatorCheck::check(const MatchFinder::MatchResult& Result) {
    const auto& sourceManager = *Result.SourceManager;

    const auto* ddlCoordinatorMethod =
        Result.Nodes.getNodeAs<clang::CXXMethodDecl>("ddl_coordinator_method");
    if (ddlCoordinatorMethod) {
        auto filename = sourceManager.getFilename(ddlCoordinatorMethod->getLocation());
        files[filename].hasCoordinatorMethod = true;
    }

    const auto* invariantCall = Result.Nodes.getNodeAs<clang::CallExpr>("invariant_call");
    if (invariantCall) {
        auto filename = sourceManager.getPresumedLoc(invariantCall->getBeginLoc()).getFilename();
        files[filename].invariantCalls.push_back(invariantCall);
    }
}

void InvariantDDLCoordinatorCheck::onEndOfTranslationUnit() {
    for (const auto& [_, fileContext] : files) {
        if (fileContext.hasCoordinatorMethod) {
            for (const auto* invariantCall : fileContext.invariantCalls) {
                diag(invariantCall->getBeginLoc(),
                     "Use 'tassert' instead of 'invariant' in DDL coordinator code. "
                     "Invariants in DDL coordinators are prone to crash loops.");
            }
        }
    }

    // Reset state for the next translation unit
    files.clear();
}
}  // namespace mongo::tidy
