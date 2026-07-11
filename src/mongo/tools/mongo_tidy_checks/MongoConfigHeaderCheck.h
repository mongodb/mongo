// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoConfigHeaderCheck is a custom clang-tidy check for detecting
 * the usage of MONGO_CONFIG_* macros without prior inclusion of "mongo/config.h" header.
 *
 * It extends ClangTidyCheck and overrides the registerPPCallbacks function. The registerPPCallbacks
 * function adds a custom Preprocessor callback class (MongoConfigHeaderPPCallbacks) to handle
 * preprocessor events and detect MONGO_CONFIG_* macro usages without proper inclusion of
 * "mongo/config.h".
 */
class MongoConfigHeaderCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoConfigHeaderCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderPP) override;
};

}  // namespace mongo::tidy
