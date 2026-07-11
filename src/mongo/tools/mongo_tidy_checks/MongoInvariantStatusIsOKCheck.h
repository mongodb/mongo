// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>
namespace mongo::tidy {

/**
 * Check for any instances of invariant(status.isOK()).
 * Overrides the default registerMatchers function to add matcher to match the
 * usage of invariant(status.isOK()). Overrides the default check function to
 * flag the uses of invariant(status.isOK()) to enforce usage of invariant(status) instead.
 */
class MongoInvariantStatusIsOKCheck : public clang::tidy::ClangTidyCheck {

public:
    MongoInvariantStatusIsOKCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace mongo::tidy
