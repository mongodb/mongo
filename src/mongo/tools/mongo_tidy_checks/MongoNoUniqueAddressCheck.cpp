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

#include "MongoNoUniqueAddressCheck.h"

#include <clang/Lex/Lexer.h>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

void MongoNoUniqueAddressCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // This matcher finds field members that have the attribute [[no_unique_address]].
    Finder->addMatcher(fieldDecl(hasAttr(attr::NoUniqueAddress)).bind("no_unique_address_var"),
                       this);
}

void MongoNoUniqueAddressCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {
    const auto* matchedNoUniqueAddressVar =
        Result.Nodes.getNodeAs<FieldDecl>("no_unique_address_var");

    if (matchedNoUniqueAddressVar) {
        // See if this [[no_unique_address]] attribute was expanded from the platform-generic
        // macro.
        auto& sm = Result.Context->getSourceManager();
        auto& lo = Result.Context->getLangOpts();
        auto attrs = matchedNoUniqueAddressVar->getAttrs();
        for (auto& attr : attrs) {
            if (attr->getKind() == attr::NoUniqueAddress) {
                auto charRange =
                    Lexer::getAsCharRange(sm.getExpansionRange(attr->getLocation()), sm, lo);
                auto originalMacro = Lexer::getSourceText(charRange, sm, lo);
                if (originalMacro == "MONGO_COMPILER_NO_UNIQUE_ADDRESS") {
                    return;
                }
            }
        }

        diag(matchedNoUniqueAddressVar->getBeginLoc(),
             "Illegal use of [[no_unique_address]] due to being supported only by "
             "[[msvc::no_unique_address]] when compiling with MSVC. Please use the "
             "platform-independent MONGO_COMPILER_NO_UNIQUE_ADDRESS.");
    }
}
}  // namespace mongo::tidy
