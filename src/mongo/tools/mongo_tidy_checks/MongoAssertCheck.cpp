// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoAssertCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

// Callbacks for handling preprocessor events
class MongoAssertMacroPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoAssertMacroPPCallbacks(MongoAssertCheck& Check) : Check(Check) {}

    void MacroExpands(const clang::Token& MacroNameTok,
                      const clang::MacroDefinition& MD,
                      SourceRange Range,
                      const MacroArgs* Args) override {
        if (MacroNameTok.getIdentifierInfo()->getName() == "assert") {
            Check.diag(Range.getBegin(),
                       "Illegal use of the bare assert macro, use a macro function from "
                       "assert_util.h instead");
        }
    }

private:
    MongoAssertCheck& Check;
};

MongoAssertCheck::MongoAssertCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoAssertCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                           clang::Preprocessor* PP,
                                           clang::Preprocessor* ModuleExpanderpp) {
    PP->addPPCallbacks(std::make_unique<MongoAssertMacroPPCallbacks>(*this));
}

}  // namespace mongo::tidy
