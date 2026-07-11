// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo {
namespace tidy {
/**
 * check for new instances of UninteruptibleLockGuard
 * Overrides the default registerMatchers function to add matcher to match the
 * new instance of UninteruptibleLockGuard. overrides the default check function to
 * flag the uses of UninteruptibleLockGuard since it does not comply with the design
 * requirements of MongoDB, they should be flagged so new instances receive extra
 * scrutiny from authors and code reviewers.
 *
 * TODO SERVER-68868: Remove this class once ULG doesn't exist.
 */
class MongoUninterruptibleLockGuardCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoUninterruptibleLockGuardCheck(clang::StringRef Name,
                                       clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace tidy
}  // namespace mongo
