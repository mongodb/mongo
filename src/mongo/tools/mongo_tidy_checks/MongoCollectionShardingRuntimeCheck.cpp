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

#include "MongoCollectionShardingRuntimeCheck.h"

#include <clang-tidy/utils/OptionsUtils.h>
#include <filesystem>

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

MongoCollectionShardingRuntimeCheck::MongoCollectionShardingRuntimeCheck(
    StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context),
      exceptionDirs(clang::tidy::utils::options::parseStringList(
          Options.get("exceptionDirs", "src/mongo/db/s"))),
      exceptionFiles(clang::tidy::utils::options::parseStringList(
          Options.get("exceptionFiles", "src/mongo/db/exec/query_shard_server_test_fixture.cpp"))) {
    // Accept relative paths
    int origLength = static_cast<int>(exceptionDirs.size());
    for (int i = 0; i < origLength; i++) {
        exceptionDirs.push_back(std::filesystem::current_path() / exceptionDirs[i]);
    }

    origLength = static_cast<int>(exceptionFiles.size());
    for (int i = 0; i < origLength; i++) {
        exceptionFiles.push_back(std::filesystem::current_path() / exceptionFiles[i]);
    }
}

void MongoCollectionShardingRuntimeCheck::registerMatchers(MatchFinder* Finder) {
    // Match instances of the CollectionShardingRuntime class
    Finder->addMatcher(
        varDecl(hasType(cxxRecordDecl(hasName("CollectionShardingRuntime")))).bind("instanceCall"),
        this);

    // Match function calls made by the CollectionShardingRuntime class
    Finder->addMatcher(callExpr(callee(functionDecl(hasParent(
                                    cxxRecordDecl(hasName("CollectionShardingRuntime"))))))
                           .bind("funcCall"),
                       this);
}

void MongoCollectionShardingRuntimeCheck::check(const MatchFinder::MatchResult& Result) {
    const auto* SM = Result.SourceManager;

    if (!SM) {
        return;
    }

    // Get the location of the current source file
    const auto FileLoc = SM->getLocForStartOfFile(SM->getMainFileID());
    if (FileLoc.isInvalid()) {
        return;
    }

    // Get the current source file path
    const auto FilePath = SM->getFilename(FileLoc);
    if (FilePath.empty()) {
        return;
    }

    std::string suffix = "_test.cpp";
    // Check if FilePath ends with the suffix "_test.cpp"
    if (FilePath.size() > suffix.size() &&
        FilePath.rfind(suffix) == FilePath.size() - suffix.size()) {
        return;
    }

    // If the file path is in an exception directory, skip the check.
    for (const auto& dir : this->exceptionDirs) {
        if (FilePath.startswith(dir)) {
            return;
        }
    }

    // If the file path is one of the exception files, skip the check.
    for (const auto& file : this->exceptionFiles) {
        if (FilePath.equals(file)) {
            return;
        }
    }

    // Get the matched instance or function call
    const auto* instanceDecl = Result.Nodes.getNodeAs<VarDecl>("instanceCall");
    if (instanceDecl) {
        diag(instanceDecl->getBeginLoc(),
             "Illegal use of CollectionShardingRuntime outside of mongo/db/s/; use "
             "CollectionShardingState instead; see src/mongo/db/s/collection_sharding_state.h for "
             "details.");
    }

    const auto* funcCallExpr = Result.Nodes.getNodeAs<CallExpr>("funcCall");
    if (funcCallExpr) {
        diag(funcCallExpr->getBeginLoc(),
             "Illegal use of CollectionShardingRuntime outside of mongo/db/s/; use "
             "CollectionShardingState instead; see src/mongo/db/s/collection_sharding_state.h for "
             "details.");
    }
}

}  // namespace mongo::tidy
