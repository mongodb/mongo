// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoTraceCheck is a custom clang-tidy check for detecting
 * the usage of  TracerProvider::get and TracerProvider::initialize
 * in the source code.
 *
 * It extends ClangTidyCheck and overrides the registerMatchers
 * and check functions. The registerMatchers function adds matchers
 * to identify the usage of racerProvider::get and TracerProvider::initialize,
 * while the check function flags the matched occurrences
 */
class MongoTraceCheck : public clang::tidy::ClangTidyCheck {

public:
    MongoTraceCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace mongo::tidy
