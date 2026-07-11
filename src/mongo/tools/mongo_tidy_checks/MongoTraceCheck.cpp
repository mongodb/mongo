// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
