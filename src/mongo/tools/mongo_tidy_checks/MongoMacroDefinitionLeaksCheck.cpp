// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "MongoMacroDefinitionLeaksCheck.h"

#include <stack>

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

// Callbacks for handling preprocessor events
class MongoMacroPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoMacroPPCallbacks(MongoMacroDefinitionLeaksCheck& Check,
                                   clang::LangOptions LangOpts,
                                   const clang::SourceManager& SM)
        : Check(Check), LangOpts(LangOpts), SM(SM) {}

    // Callback for when a macro is defined
    void MacroDefined(const clang::Token& MacroNameTok, const clang::MacroDirective* MD) override {
        llvm::StringRef macroName = MacroNameTok.getIdentifierInfo()->getName();
        if (macroName == "MONGO_LOGV2_DEFAULT_COMPONENT") {
            defineUndefDiff += 1;
            lastMacroLocation = MD->getLocation();
        }
    }

    // Callback for when a macro is undefined
    void MacroUndefined(const clang::Token& MacroNameTok,
                        const clang::MacroDefinition& MD,
                        const clang::MacroDirective* Undef) override {
        llvm::StringRef macroName = MacroNameTok.getIdentifierInfo()->getName();

        if (macroName == "MONGO_LOGV2_DEFAULT_COMPONENT") {
            defineUndefDiff -= 1;
        }
    }

    // Callback for when a file is included or excluded
    void FileChanged(SourceLocation Loc,
                     FileChangeReason Reason,
                     SrcMgr::CharacteristicKind FileType,
                     FileID PrevFID) override {
        if (Reason != EnterFile && Reason != ExitFile)
            return;

        const FileEntry* CurrentFile = SM.getFileEntryForID(SM.getFileID(Loc));
        if (!CurrentFile)
            return;

        if (Reason == EnterFile) {
            // Push the file to the stack
            fileStack.push(CurrentFile->tryGetRealPathName().str());
            defineUndefDiff = 0;
        } else if (Reason == ExitFile && !fileStack.empty()) {
            // Get the top file from the stack
            std::string currentFileName = fileStack.top();
            fileStack.pop();
            if (defineUndefDiff != 0) {
                Check.diag(lastMacroLocation, "Missing #undef 'MONGO_LOGV2_DEFAULT_COMPONENT'");
            }
        }
    }

private:
    MongoMacroDefinitionLeaksCheck& Check;
    clang::LangOptions LangOpts;
    const clang::SourceManager& SM;
    int defineUndefDiff = 0;
    clang::SourceLocation lastMacroLocation;
    std::stack<std::string> fileStack;
};

MongoMacroDefinitionLeaksCheck::MongoMacroDefinitionLeaksCheck(
    llvm::StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoMacroDefinitionLeaksCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                         clang::Preprocessor* PP,
                                                         clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(::std::make_unique<MongoMacroPPCallbacks>(*this, getLangOpts(), SM));
}

}  // namespace mongo::tidy
