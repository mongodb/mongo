// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoBannedNamesCheck is a custom clang-tidy check for detecting
 * the usage of listed names from the std or boost libraries (std,
 * boost, or global namespace) in the source code.
 */
class MongoBannedNamesCheck : public clang::tidy::ClangTidyCheck {
public:
    using clang::tidy::ClangTidyCheck::ClangTidyCheck;
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
    void checkNamespace(clang::SourceLocation loc, llvm::StringRef name);
};
}  // namespace mongo::tidy
