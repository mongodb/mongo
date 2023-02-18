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

#include "MongoStdOptionalCheck.h"
#include <iostream>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoStdOptionalCheck::MongoStdOptionalCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

/**
Matches declarations whose declaration context is the C++ standard library
namespace std.

Note that inline namespaces are silently ignored during the lookup since
both libstdc++ and libc++ are known to use them for versioning purposes.

Given:
\code
    namespace ns {
    struct my_type {};
    using namespace std;
    }

    using std::optional;
    using ns:my_type;
    using ns::optional;
 \code

usingDecl(hasAnyUsingShadowDecl(hasTargetDecl(isFromStdNamespace())))
matches "using std::optional" and "using ns::optional".
*/
AST_MATCHER(Decl, isFromStdNamespace) {
    const DeclContext* D = Node.getDeclContext();

    while (D->isInlineNamespace())
        D = D->getParent();

    if (!D->isNamespace() || !D->getParent()->isTranslationUnit())
        return false;

    const IdentifierInfo* Info = cast<NamespaceDecl>(D)->getIdentifier();

    return (Info && Info->isStr("std"));
}

void MongoStdOptionalCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {

    // match using std::optional;
    Finder->addMatcher(usingDecl(hasAnyUsingShadowDecl(hasTargetDecl(
                                     allOf(hasName("optional"), isFromStdNamespace()))))
                           .bind("decl_optional"),
                       this);

    // match parameter decl, variable Decl, Field Decl, Reference Decl, Template Decl regarding
    // std::optional
    Finder->addMatcher(loc(templateSpecializationType(hasDeclaration(
                               namedDecl(hasName("optional"), isFromStdNamespace()))))
                           .bind("loc_optional"),
                       this);
}

void MongoStdOptionalCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    const auto* decl_match = Result.Nodes.getNodeAs<UsingDecl>("decl_optional");
    const auto* loc_match = Result.Nodes.getNodeAs<TypeLoc>("loc_optional");

    if (decl_match) {
        diag(decl_match->getBeginLoc(), "Use of std::optional, use boost::optional instead. ");
    }
    if (loc_match && !loc_match->getBeginLoc().isInvalid()) {
        diag(loc_match->getBeginLoc(), "Use of std::optional, use boost::optional instead. ");
    }
}

}  // namespace mongo::tidy
