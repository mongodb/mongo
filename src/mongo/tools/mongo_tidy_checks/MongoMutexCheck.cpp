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

#include "MongoMutexCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoMutexCheck::MongoMutexCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoMutexCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // TODO: SERVER-74929 Remove the NOLINT comment below after we remove _check_for_mongo_polyfill
    // check from simplecpplint.py
    // This matcher finds variable declarations (outside of structs/classes) with a type of either
    // std::mutex or stdx::mutex. It works by matching variable declarations whose type, when
    // reduced to its canonical form, has a declaration named "::std::mutex".
    Finder->addMatcher(varDecl(hasType(qualType(hasCanonicalType(
                                   hasDeclaration(namedDecl(hasName("::std::mutex")))))))  // NOLINT
                           .bind("mutex_var"),
                       this);

    // This matcher finds field declarations (inside structs/classes) with a type of either
    // std::mutex or stdx::mutex.
    Finder->addMatcher(fieldDecl(hasType(qualType(hasCanonicalType(hasDeclaration(
                                     namedDecl(hasName("::std::mutex")))))))  // NOLINT
                           .bind("mutex_field"),
                       this);
}

void MongoMutexCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    // Get the matched variable declaration of type std::mutex or stdx::mutex
    const auto* matchedMutexVar = Result.Nodes.getNodeAs<VarDecl>("mutex_var");
    if (matchedMutexVar) {
        diag(matchedMutexVar->getBeginLoc(),
             "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h "
             "instead.");
    }

    // Get the matched field declaration of type std::mutex or stdx::mutex
    const auto* matchedMutexField = Result.Nodes.getNodeAs<FieldDecl>("mutex_field");
    if (matchedMutexField) {
        diag(matchedMutexField->getBeginLoc(),
             "Illegal use of prohibited stdx::mutex, use mongo::Mutex from mongo/platform/mutex.h "
             "instead.");
    }
}
}  // namespace mongo::tidy
