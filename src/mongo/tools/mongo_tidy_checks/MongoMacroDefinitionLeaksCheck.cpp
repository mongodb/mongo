/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "MongoMacroDefinitionLeaksCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <stack>

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
