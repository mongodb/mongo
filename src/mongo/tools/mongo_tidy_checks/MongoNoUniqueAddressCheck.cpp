// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
