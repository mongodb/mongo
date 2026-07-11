// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * check for new instances of Volatile
 * Overrides the default registerMatchers function to add matcher to match the
 * use of volatile. overrides the default check function to flag the uses of volatile
 */
class MongoVolatileCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoVolatileCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace mongo::tidy
