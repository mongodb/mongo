// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "MongoCctypeCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

namespace {

class MongoCctypePPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoCctypePPCallbacks(MongoCctypeCheck& Check, clang::LangOptions LangOpts)
        : Check(Check), LangOpts(LangOpts) {}

    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token& IncludeTok,
                            llvm::StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            clang::OptionalFileEntryRef File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* SuggestedModule,
                            bool ModuleImported,
                            clang::SrcMgr::CharacteristicKind FileType) override {

        // match following cases
        // #include <cctype>
        // #include <ctype.h>
        if (FileName == "cctype" || FileName == "ctype.h") {
            std::string Replacement = "\"mongo/util/ctype.h\"";
            Check.diag(FilenameRange.getBegin(),
                       "Use of prohibited %0%1%2 header, use \"mongo/util/ctype.h\"")
                << (IsAngled ? "<" : "\"") << FileName << (IsAngled ? ">" : "\"")
                << clang::FixItHint::CreateReplacement(FilenameRange.getAsRange(), Replacement);
        }
    }

private:
    MongoCctypeCheck& Check;
    clang::LangOptions LangOpts;
};

}  // namespace

MongoCctypeCheck::MongoCctypeCheck(llvm::StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoCctypeCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                           clang::Preprocessor* PP,
                                           clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(::std::make_unique<MongoCctypePPCallbacks>(*this, getLangOpts()));
}

}  // namespace mongo::tidy
