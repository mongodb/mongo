// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoAssertCheck is a custom clang-tidy check for detecting
 * the usage of nonmongo assert in the source code.
 *
 * It extends ClangTidyCheck and overrides the registerMatchers
 * and check functions. The registerMatchers function adds matchers
 * to identify the usage of nongmongo assert, while
 * the check function flags the matched occurrences
 */
class MongoAssertCheck : public clang::tidy::ClangTidyCheck {

public:
    MongoAssertCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderpp) override;
};

}  // namespace mongo::tidy
