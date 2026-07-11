// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo {
namespace tidy {
/**
 * MongoFCVConstantCheck is a custom clang-tidy check for detecting the usage of
 * comparing FCV using the FeatureCompatibilityVersion enums, e.g.
 * FeatureCompatibilityVersion::kVersion_X_Y.
 *
 * It extends ClangTidyCheck and overrides the registerMatchers
 * and check functions. The registerMatchers function adds matchers
 * to identify the usage of nongmongo assert, while
 * the check function flags the matched occurrences
 */
class MongoFCVConstantCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoFCVConstantCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace tidy
}  // namespace mongo
