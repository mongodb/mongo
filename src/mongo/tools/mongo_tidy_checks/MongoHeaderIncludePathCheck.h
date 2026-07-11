// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
    Overrides the default PPCallback class to primarily override
    the InclusionDirective call which is called for each include included. This
    allows the chance to evaluate specifically the include and determine whether
    it is considered a "mongo" include or not and if it is using the appropriate include style.
*/
class MongoHeaderIncludePathCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoHeaderIncludePathCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void storeOptions(clang::tidy::ClangTidyOptions::OptionMap& Opts) override;
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderPP) override;
    // used to store option `mongoSourceDirs`; supports both absolute and relative paths
    std::vector<llvm::StringRef> mongoSourceDirs;

    // The path component that denotes the repo's root source directory (default: "src").
    // Used to compute canonical include paths "from src/".
    std::string srcRootComponent;
};

}  // namespace mongo::tidy
