// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
    Overrides the default PPCallback class to primarly override
    the InclusionDirective call which is called for each include. This
    allows the chance to check whether <cctype> or <ctype.h> included or not
*/
class MongoCctypeCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoCctypeCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderPP) override;
};

}  // namespace mongo::tidy
