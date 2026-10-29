// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo {
namespace tidy {

/**
 * MongoBypassDatabaseMetadataAccessCheck is a custom clang-tidy check for detecting the usage of
 * BypassDatabaseMetadataAccess class in the source code.
 */
class MongoBypassDatabaseMetadataAccessCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoBypassDatabaseMetadataAccessCheck(clang::StringRef Name,
                                           clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace tidy
}  // namespace mongo
