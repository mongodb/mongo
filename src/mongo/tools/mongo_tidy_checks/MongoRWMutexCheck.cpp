// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoRWMutexCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoRWMutexCheck::MongoRWMutexCheck(StringRef name, clang::tidy::ClangTidyContext* context)
    : ClangTidyCheck(name, context) {}

void MongoRWMutexCheck::registerMatchers(MatchFinder* finder) {
    auto hasTargetType = hasType(
        qualType(hasCanonicalType(hasDeclaration(namedDecl(hasName("WriteRarelyRWMutex"))))));
    finder->addMatcher(varDecl(hasTargetType).bind("mutex_var"), this);
    finder->addMatcher(fieldDecl(hasTargetType).bind("mutex_field"), this);
}

void MongoRWMutexCheck::check(const MatchFinder::MatchResult& result) {
    if (const auto matchedVar = result.Nodes.getNodeAs<VarDecl>("mutex_var")) {
        diag(matchedVar->getBeginLoc(),
             "Prefer using other mutex types over `WriteRarelyRWMutex`.");
    }

    if (const auto matchedField = result.Nodes.getNodeAs<FieldDecl>("mutex_field")) {
        diag(matchedField->getBeginLoc(),
             "Prefer using other mutex types over `WriteRarelyRWMutex`.");
    }
}

}  // namespace mongo::tidy
