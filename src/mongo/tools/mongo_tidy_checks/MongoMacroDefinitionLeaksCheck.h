// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoMacroDefinitionLeaksCheck is a custom clang-tidy check for detecting
 * the imbalance between the definitions and undefinitions of the macro
 * "MONGO_LOGV2_DEFAULT_COMPONENT" in the same file.
 *
 * It extends ClangTidyCheck and overrides the registerPPCallbacks function. The registerPPCallbacks
 * function adds a custom Preprocessor callback class (MongoMacroPPCallbacks) to handle
 * preprocessor events and detect an imbalance in the definitions and undefinitions of the
 * "MONGO_LOGV2_DEFAULT_COMPONENT" macro within each file.
 *
 * If a .h or .hpp file is found to have a non-zero difference between definitions and undefinitions
 * of the "MONGO_LOGV2_DEFAULT_COMPONENT" macro, it's considered a leak and the check raises a
 * diagnostic message pointing out the location of the last macro definition.
 */
class MongoMacroDefinitionLeaksCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoMacroDefinitionLeaksCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderPP) override;
};

}  // namespace mongo::tidy
