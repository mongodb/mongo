/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "MongoInvariantStatusIsOKCheck.h"

#include <iostream>

#include "MongoTidyUtils.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoInvariantStatusIsOKCheck::MongoInvariantStatusIsOKCheck(StringRef Name,
                                                             clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoInvariantStatusIsOKCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {

    Finder->addMatcher(
        callExpr(callee(functionDecl(anyOf(hasName("invariantWithContextAndLocation"),
                                           hasName("invariantWithLocation")))),
                 hasArgument(0,
                             cxxMemberCallExpr(on(hasType(cxxRecordDecl(anyOf(
                                                   hasName("Status"), hasName("StatusWith"))))),
                                               callee(cxxMethodDecl(hasName("isOK"))))))
            .bind("invariant_call"),
        this);
}

void MongoInvariantStatusIsOKCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    const auto* invariantCall = Result.Nodes.getNodeAs<CallExpr>("invariant_call");

    if (invariantCall) {
        diag(invariantCall->getBeginLoc(),
             "Found invariant(status.isOK()) or dassert(status.isOK()), use invariant(status) for "
             "better diagnostics");
    }
}

}  // namespace mongo::tidy
