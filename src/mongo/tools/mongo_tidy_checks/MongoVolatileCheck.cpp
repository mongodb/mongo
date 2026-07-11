// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
             "Illegal use of the volatile storage keyword, use Atomic instead from "
             "\"mongo/platform/atomic.h\"");
    }
    if (field_match) {
        diag(field_match->getBeginLoc(),
             "Illegal use of the volatile storage keyword, use Atomic instead from "
             "\"mongo/platform/atomic.h\"");
    }
}

}  // namespace mongo::tidy
