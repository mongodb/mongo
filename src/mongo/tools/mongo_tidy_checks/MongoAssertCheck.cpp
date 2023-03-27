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

#include <clang/Lex/Lexer.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoAssertCheck::MongoAssertCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

// Custom AST Matcher that checks if an expression is an 'assert' macro expansion
AST_MATCHER(Expr, isAssertMacroExpansion) {

    // Return false if the expression is not a macro expansion
    if (!Node.getBeginLoc().isMacroID()) {
        return false;
    }

    auto& SM = Finder->getASTContext().getSourceManager();
    auto MacroLoc = SM.getImmediateMacroCallerLoc(Node.getBeginLoc());

    // Get the name of the macro being expanded
    llvm::StringRef MacroSpellingRef =
        clang::Lexer::getImmediateMacroName(MacroLoc, SM, Finder->getASTContext().getLangOpts());
    std::string Spelling = MacroSpellingRef.str();

    if (Spelling == "assert") {
        // Check if the file name contains "assert.h"
        // <cassert> will also be caught because it redirects to assert.h
        auto FileName = SM.getFilename(SM.getSpellingLoc(MacroLoc));
        return llvm::StringRef(FileName).contains("assert.h");
    }
    return false;
}

void MongoAssertCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {

    // Add the custom matcher 'isAssertMacroExpansion' to the MatchFinder
    Finder->addMatcher(expr(isAssertMacroExpansion()).bind("assertMacroExpansion"), this);
}

void MongoAssertCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    // Check assert macro expansion
    const auto* AssertMacroExpansion = Result.Nodes.getNodeAs<Expr>("assertMacroExpansion");
    if (AssertMacroExpansion) {
        diag(AssertMacroExpansion->getBeginLoc(),
             "Illegal use of the bare assert function, use a function from assert_util.h instead");
    }
}

}  // namespace mongo::tidy
