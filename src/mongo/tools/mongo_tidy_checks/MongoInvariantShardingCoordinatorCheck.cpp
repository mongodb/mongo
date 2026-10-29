// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoInvariantShardingCoordinatorCheck.h"

using namespace clang::ast_matchers;

namespace mongo::tidy {

InvariantShardingCoordinatorCheck::InvariantShardingCoordinatorCheck(
    clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void InvariantShardingCoordinatorCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(cxxMethodDecl(ofClass(cxxRecordDecl(anyOf(
                                         isSameOrDerivedFrom(hasName("ShardingCoordinator")),
                                         isSameOrDerivedFrom(hasName("ConfigsvrCoordinator"))))))
                           .bind("sharding_coordinator_method"),
                       this);

    // Note: invariant() is a macro, translated by the preprocessor to these functions.
    Finder->addMatcher(
        callExpr(callee(functionDecl(anyOf(hasName("invariantWithContextAndLocation"),
                                           hasName("invariantWithLocation")))))
            .bind("invariant_call"),
        this);
}

void InvariantShardingCoordinatorCheck::check(const MatchFinder::MatchResult& Result) {
    const auto& sourceManager = *Result.SourceManager;

    const auto* shardingCoordinatorMethod =
        Result.Nodes.getNodeAs<clang::CXXMethodDecl>("sharding_coordinator_method");
    if (shardingCoordinatorMethod) {
        auto filename = sourceManager.getFilename(shardingCoordinatorMethod->getLocation());
        files[filename].hasCoordinatorMethod = true;
    }

    const auto* invariantCall = Result.Nodes.getNodeAs<clang::CallExpr>("invariant_call");
    if (invariantCall) {
        auto filename = sourceManager.getPresumedLoc(invariantCall->getBeginLoc()).getFilename();
        files[filename].invariantCalls.push_back(invariantCall);
    }
}

void InvariantShardingCoordinatorCheck::onEndOfTranslationUnit() {
    for (const auto& [_, fileContext] : files) {
        if (fileContext.hasCoordinatorMethod) {
            for (const auto* invariantCall : fileContext.invariantCalls) {
                diag(invariantCall->getBeginLoc(),
                     "Use 'tassert' instead of 'invariant' in sharding coordinator code. "
                     "Invariants in sharding coordinators are prone to crash loops.");
            }
        }
    }

    // Reset state for the next translation unit
    files.clear();
}
}  // namespace mongo::tidy
