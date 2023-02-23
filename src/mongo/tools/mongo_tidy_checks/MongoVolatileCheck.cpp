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

#include "MongoVolatileCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoVolatileCheck::MongoVolatileCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoVolatileCheck::registerMatchers(MatchFinder* Finder) {

    // Matcher for finding all instances of variables volatile
    Finder->addMatcher(varDecl(hasType(isVolatileQualified())).bind("var_volatile"), this);

    // Matcher for finding all instances of field volatile
    Finder->addMatcher(fieldDecl(hasType(isVolatileQualified())).bind("field_volatile"), this);
}

void MongoVolatileCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* var_match = Result.Nodes.getNodeAs<VarDecl>("var_volatile");
    const auto* field_match = Result.Nodes.getNodeAs<FieldDecl>("field_volatile");

    if (var_match) {
        diag(var_match->getBeginLoc(),
             "Illegal use of the volatile storage keyword, use AtomicWord instead from "
             "\"mongo/platform/atomic_word.h\"");
    }
    if (field_match) {
        diag(field_match->getBeginLoc(),
             "Illegal use of the volatile storage keyword, use AtomicWord instead from "
             "\"mongo/platform/atomic_word.h\"");
    }
}

}  // namespace mongo::tidy
