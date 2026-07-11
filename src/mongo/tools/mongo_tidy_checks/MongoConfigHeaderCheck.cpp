// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
        if (macroName.starts_with("MONGO_CONFIG_")) {
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
                            clang::OptionalFileEntryRef File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* SuggestedModule,
                            bool ModuleImported,
                            clang::SrcMgr::CharacteristicKind FileType) override {

        if (FileName == "mongo/config.h") {
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
