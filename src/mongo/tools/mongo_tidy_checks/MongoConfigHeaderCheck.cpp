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


#include "MongoConfigHeaderCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

class MongoConfigHeaderPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoConfigHeaderPPCallbacks(MongoConfigHeaderCheck& Check,
                                          clang::LangOptions LangOpts,
                                          const clang::SourceManager& SM)
        : Check(Check), LangOpts(LangOpts), SM(SM) {}

    // Function to check the usage of MONGO_CONFIG_* macros
    void checkMacroUsage(clang::SourceLocation Loc, const clang::Token& MacroNameTok) {
        if (ConfigHeaderIncluded)
            return;
        llvm::StringRef macroName = MacroNameTok.getIdentifierInfo()->getName();
        if (macroName.startswith("MONGO_CONFIG_")) {
            Check.diag(Loc, "MONGO_CONFIG define used without prior inclusion of config.h");
        }
    }

    // Callback function for handling macro definitions
    void MacroDefined(const clang::Token& MacroNameTok, const clang::MacroDirective* MD) override {
        if (ConfigHeaderIncluded)
            return;
        checkMacroUsage(MD->getLocation(), MacroNameTok);
    }

    // Callback function for handling #ifdef directives
    void Ifdef(clang::SourceLocation Loc,
               const clang::Token& MacroNameTok,
               const clang::MacroDefinition& MD) override {
        if (ConfigHeaderIncluded)
            return;
        checkMacroUsage(Loc, MacroNameTok);
    }

    // Callback function for handling #ifndef directives
    void Ifndef(clang::SourceLocation Loc,
                const clang::Token& MacroNameTok,
                const clang::MacroDefinition& MD) override {
        if (ConfigHeaderIncluded)
            return;
        checkMacroUsage(Loc, MacroNameTok);
    }

    // Callback function for handling #if directives
    void If(clang::SourceLocation Loc,
            clang::SourceRange ConditionRange,
            clang::PPCallbacks::ConditionValueKind ConditionValue) override {

        if (ConfigHeaderIncluded)
            return;

        // Get the beginning and end locations of the condition in the #if directive
        clang::SourceLocation Start = ConditionRange.getBegin();
        clang::SourceLocation End = ConditionRange.getEnd();

        // Get the source text of the condition in the #if directive
        bool Invalid = false;
        llvm::StringRef ConditionText = Lexer::getSourceText(
            CharSourceRange::getTokenRange(Start, End), SM, clang::LangOptions(), &Invalid);

        if (!Invalid) {
            if (ConditionText.contains("MONGO_CONFIG_")) {
                Check.diag(Loc, "MONGO_CONFIG define used without prior inclusion of config.h");
            }
        }
    }

    // Callback function for handling #include directives
    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token& IncludeTok,
                            llvm::StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            const clang::FileEntry* File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* Imported,
                            clang::SrcMgr::CharacteristicKind FileType) override {

        if (FileName.equals("mongo/config.h")) {
            ConfigHeaderIncluded = true;
        }
    }

private:
    MongoConfigHeaderCheck& Check;
    clang::LangOptions LangOpts;
    const clang::SourceManager& SM;
    bool ConfigHeaderIncluded = false;
};

MongoConfigHeaderCheck::MongoConfigHeaderCheck(llvm::StringRef Name,
                                               clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoConfigHeaderCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                 clang::Preprocessor* PP,
                                                 clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(::std::make_unique<MongoConfigHeaderPPCallbacks>(*this, getLangOpts(), SM));
}

}  // namespace mongo::tidy
