// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoBypassDatabaseMetadataAccessCheck.h"

namespace mongo {
namespace tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoBypassDatabaseMetadataAccessCheck::MongoBypassDatabaseMetadataAccessCheck(
    StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoBypassDatabaseMetadataAccessCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(varDecl(hasType(cxxRecordDecl(hasName("BypassDatabaseMetadataAccessCheck"))))
                           .bind("BypassDatabaseMetadataAccessCheckDec"),
                       this);
}

void MongoBypassDatabaseMetadataAccessCheck::check(const MatchFinder::MatchResult& Result) {
    if (const auto matchedVar =
            Result.Nodes.getNodeAs<VarDecl>("BypassDatabaseMetadataAccessCheckDec")) {
        diag(matchedVar->getBeginLoc(),
             "Potentially incorrect use of BypassDatabaseMetadataAccessCheck: operations that "
             "modify the database metadata must acquire the critical section, and operations that "
             "read the database metadata must ensure that no one else is holding the critical "
             "section. Review carefully, and if the use is warranted, add NOLINT with a comment "
             "explaining why.");
        return;
    }
}

}  // namespace tidy
}  // namespace mongo
