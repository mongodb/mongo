// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * `MongoRWMutexCheck` is a custom clang-tidy check for detecting the usage of mutex types defined
 * in `platform/rwmutex.h`. It extends `ClangTidyCheck` and overrides the `registerMatchers` and
 * `check` functions. The `registerMatchers` function adds matchers to identify the usage of
 * 'WriteRarelyRWMutex', while the `check` function flags the matched occurrences.
 */
class MongoRWMutexCheck : public clang::tidy::ClangTidyCheck {

public:
    MongoRWMutexCheck(clang::StringRef name, clang::tidy::ClangTidyContext* context);
    void registerMatchers(clang::ast_matchers::MatchFinder* finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
};

}  // namespace mongo::tidy
