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
