// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * A clang-tidy check that bans the usage of internal catalog classes such as AutoGetCollection or
 * CollectionCatalog from certain modules.
 */
class MongoBannedCatalogAccessFromQueryCodeCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoBannedCatalogAccessFromQueryCodeCheck(clang::StringRef Name,
                                               clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace mongo::tidy
