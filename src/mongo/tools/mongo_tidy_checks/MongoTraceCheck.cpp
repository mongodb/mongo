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

#include "MongoTraceCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoTraceCheck::MongoTraceCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoTraceCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // Matcher for TracerProvider::get and TracerProvider::initialize
    Finder->addMatcher(callExpr(anyOf(callee(functionDecl(hasName("TracerProvider::get"))),
                                      callee(functionDecl(hasName("TracerProvider::initialize")))))
                           .bind("tracing_support"),
                       this);
}

void MongoTraceCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    // Get the matched TracerProvider::get and TracerProvider::initialize
    const auto* matchedTraceSupport = Result.Nodes.getNodeAs<CallExpr>("tracing_support");
    if (matchedTraceSupport) {
        diag(matchedTraceSupport->getBeginLoc(),
             "Illegal use of prohibited tracing support, this is only for local development use "
             "and should not be committed.");
    }
}

}  // namespace mongo::tidy
