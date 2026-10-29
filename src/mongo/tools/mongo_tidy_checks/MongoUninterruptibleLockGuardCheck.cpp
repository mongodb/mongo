// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoUninterruptibleLockGuardCheck.h"


namespace mongo {
namespace tidy {

namespace {
static constexpr auto kBannedMember = "uninterruptibleLocksRequested_DO_NOT_USE";

AST_MATCHER(clang::CXXMemberCallExpr, matchesForbiddenCall) {
    if (auto memberDecl = Node.getMethodDecl()) {
        if (auto identifierInfo = memberDecl->getDeclName().getAsIdentifierInfo()) {
            return identifierInfo->getName() == kBannedMember;
        }
    }
    return false;
}
}  // namespace

using namespace clang;
using namespace clang::ast_matchers;

MongoUninterruptibleLockGuardCheck::MongoUninterruptibleLockGuardCheck(
    StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoUninterruptibleLockGuardCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(varDecl(hasType(cxxRecordDecl(hasName("UninterruptibleLockGuard"))))
                           .bind("UninterruptibleLockGuardDec"),
                       this);
    Finder->addMatcher(
        cxxMemberCallExpr(allOf(thisPointerType(cxxRecordDecl(hasName("OperationContext"))),
                                matchesForbiddenCall()))
            .bind("UninterruptibleMemberCall"),
        this);
}

void MongoUninterruptibleLockGuardCheck::check(const MatchFinder::MatchResult& Result) {
    if (const auto* matchUninterruptibleLockGuardDecl =
            Result.Nodes.getNodeAs<VarDecl>("UninterruptibleLockGuardDec")) {
        diag(matchUninterruptibleLockGuardDecl->getBeginLoc(),
             "Potentially incorrect use of UninterruptibleLockGuard, "
             "the programming model inside MongoDB requires that all operations be interruptible. "
             "Review with care and if the use is warranted, add NOLINT and a comment explaining "
             "why.");
        return;
    }
    if (const auto* matchedOpCtxCall =
            Result.Nodes.getNodeAs<CXXMemberCallExpr>("UninterruptibleMemberCall")) {
        diag(matchedOpCtxCall->getBeginLoc(),
             "Potentially incorrect use of "
             "OperationContext::uninterruptibleLocksRequested_DO_NOT_USE, this is a legacy "
             "interruption mechanism that makes lock acquisition ignore interrupts. Please ensure "
             "this use is warranted and if so add a NOLINT comment explaining why. Please also add "
             "Service Arch to the PR so that we can verify this is necessary and there are no "
             "alternative workarounds.");
        return;
    }
}

}  // namespace tidy
}  // namespace mongo
