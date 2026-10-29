// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoBannedCatalogAccessFromQueryCodeCheck.h"

#include <array>
#include <string_view>

using namespace clang;
using namespace clang::ast_matchers;

namespace mongo::tidy {

MongoBannedCatalogAccessFromQueryCodeCheck::MongoBannedCatalogAccessFromQueryCodeCheck(
    clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoBannedCatalogAccessFromQueryCodeCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        varDecl(hasType(cxxRecordDecl(hasName("AutoGetCollection")))).bind("AutoGetCollectionDec"),
        this);
    Finder->addMatcher(
        callExpr(callee(cxxMethodDecl(ofClass(cxxRecordDecl(hasName("CollectionCatalog"))),
                                      isStaticStorageClass())))
            .bind("CollectionCatalogUsage"),
        this);
}

void MongoBannedCatalogAccessFromQueryCodeCheck::check(const MatchFinder::MatchResult& Result) {
    const auto& sourceManager = *Result.SourceManager;
    // Get the location of the current source file
    const auto filePath = sourceManager.getFilename(
        sourceManager.getLocForStartOfFile(sourceManager.getMainFileID()));

    // Only check on the file paths belonging to modules that are forbidden to use
    // AutoGetCollection and CollectionCatalog.
    static constexpr std::array<std::string_view, 2> forbiddenDirs = {
        "src/mongo/db/query/", "src/mongo/tools/mongo_tidy_checks/tests/"};
    if (std::none_of(forbiddenDirs.begin(),
                     forbiddenDirs.end(),
                     [&filePath](const std::string_view& dir) { return filePath.contains(dir); })) {
        return;
    }

    // Ignore unit tests.
    if (filePath.ends_with("_test.cpp"))
        return;

    if (const auto matchedVar = Result.Nodes.getNodeAs<VarDecl>("AutoGetCollectionDec")) {
        diag(matchedVar->getBeginLoc(),
             "AutoGetCollection is not allowed to be used from the query modules. Use ShardRole "
             "CollectionAcquisitions instead.");
    }

    if (const auto matchedCall = Result.Nodes.getNodeAs<CallExpr>("CollectionCatalogUsage")) {
        diag(matchedCall->getBeginLoc(),
             "CollectionCatalog is not allowed to be used from the query modules. Use ShardRole "
             "CollectionAcquisitions instead.");
    }
}

}  // namespace mongo::tidy
