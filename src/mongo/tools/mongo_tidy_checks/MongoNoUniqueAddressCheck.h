// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoNoUniqueAddressCheck is a custom clang-tidy check for detecting the usage of
 * [[no_unique_address]] in the source code.
 *
 * It extends ClangTidyCheck and overrides the registerMatchers and check functions. The
 * registerMatches functions adds matchers to identify the usage of [[no_unique_address]], while
 * the check function flags the matched occurrences for further analysis or modification.
 */
class MongoNoUniqueAddressCheck : public clang::tidy::ClangTidyCheck {

public:
    using ClangTidyCheck::ClangTidyCheck;
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace mongo::tidy
