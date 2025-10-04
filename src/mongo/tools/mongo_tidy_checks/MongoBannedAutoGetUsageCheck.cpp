/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "MongoBannedAutoGetUsageCheck.h"

#include <array>
#include <string_view>

using namespace clang;
using namespace clang::ast_matchers;

namespace mongo::tidy {

MongoBannedAutoGetUsageCheck::MongoBannedAutoGetUsageCheck(clang::StringRef Name,
                                                           clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoBannedAutoGetUsageCheck::registerMatchers(MatchFinder* Finder) {
    Finder->addMatcher(
        varDecl(hasType(cxxRecordDecl(hasName("AutoGetCollection")))).bind("AutoGetCollectionDec"),
        this);
}

void MongoBannedAutoGetUsageCheck::check(const MatchFinder::MatchResult& Result) {
    const auto& sourceManager = *Result.SourceManager;
    // Get the location of the current source file
    const auto filePath = sourceManager.getFilename(
        sourceManager.getLocForStartOfFile(sourceManager.getMainFileID()));

    // Only check on the file paths belonging to modules that are forbidden to use
    // AutoGetCollection.
    static constexpr std::array<std::string_view, 2> forbiddenDirs = {
        "src/mongo/db/query/", "src/mongo/tools/mongo_tidy_checks/tests/"};
    if (std::none_of(forbiddenDirs.begin(),
                     forbiddenDirs.end(),
                     [&filePath](const std::string_view& dir) { return filePath.contains(dir); })) {
        return;
    }

    if (const auto matchedVar = Result.Nodes.getNodeAs<VarDecl>("AutoGetCollectionDec")) {
        diag(matchedVar->getBeginLoc(),
             "AutoGetCollection is not allowed to be used from the query modules. Use ShardRole "
             "CollectionAcquisitions instead.");
    }
}

}  // namespace mongo::tidy
