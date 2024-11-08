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
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoCollectionShardingRuntimeCheck is a custom clang-tidy check for detecting
 * the usage of CollectionShardingRuntime outside of the "mongo/db/s/" directory.
 *
 * It extends ClangTidyCheck and overrides the registerMatchers and check functions.
 * The registerMatchers function adds custom AST Matchers to detect the instances and function calls
 * of CollectionShardingRuntime. These matchers bind matched nodes with names for later retrieval in
 * check function.
 *
 * The check function then retrieves the nodes bound by the matchers and emit diagnostics if
 * any illegal usage is detected. An usage is considered illegal if the source file is not in the
 * exception directories (e.g., "src/mongo/db/s/") or exception files list.
 *
 */
class MongoCollectionShardingRuntimeCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoCollectionShardingRuntimeCheck(clang::StringRef Name,
                                        clang::tidy::ClangTidyContext* Context);
    void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
    std::vector<std::string> exceptionDirs;
    std::vector<std::string> exceptionFiles;
};

}  // namespace mongo::tidy
