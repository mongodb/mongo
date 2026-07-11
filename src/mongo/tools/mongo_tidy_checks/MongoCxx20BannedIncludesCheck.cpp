// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "MongoCxx20BannedIncludesCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

namespace {

class MongoCxx20BannedIncludesPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoCxx20BannedIncludesPPCallbacks(MongoCxx20BannedIncludesCheck& Check,
                                                 clang::LangOptions LangOpts)
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

        if (FileName == "coroutine" || FileName == "format") {
            Check.diag(FilenameRange.getBegin(),
                       "Use of prohibited %0%1%2 header. There are no override waivers issued for "
                       "this header. For questions please reach out to #cxx-discuss.")
                << (IsAngled ? "<" : "\"") << FileName << (IsAngled ? ">" : "\"");
        }

        if (FileName == "syncstream" || FileName == "ranges" || FileName == "barrier" ||
            FileName == "latch" || FileName == "semaphore") {
            Check.diag(FilenameRange.getBegin(),
                       "Use of prohibited %0%1%2 header. There are override waivers issued for "
                       "this header. You need to follow the process in PM-3140 to override this "
                       "warning. For questions please reach out to #cxx-discuss.")
                << (IsAngled ? "<" : "\"") << FileName << (IsAngled ? ">" : "\"");
        }
    }

private:
    MongoCxx20BannedIncludesCheck& Check;
    clang::LangOptions LangOpts;
};

}  // namespace

MongoCxx20BannedIncludesCheck::MongoCxx20BannedIncludesCheck(llvm::StringRef Name,
                                                             clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoCxx20BannedIncludesCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                        clang::Preprocessor* PP,
                                                        clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(
        ::std::make_unique<MongoCxx20BannedIncludesPPCallbacks>(*this, getLangOpts()));
}

}  // namespace mongo::tidy
