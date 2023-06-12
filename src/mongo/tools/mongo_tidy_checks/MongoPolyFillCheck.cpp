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

#include "MongoPolyFillCheck.h"

namespace mongo::tidy {

using namespace clang;
using namespace clang::ast_matchers;

std::string generateBannedNamesPattern(std::vector<std::string> polyfillRequiredNames) {

    // Generate a regex pattern from banned names
    std::string regexStr;
    for (const auto& name : polyfillRequiredNames) {
        if (!regexStr.empty()) {
            regexStr += "|";
        }
        regexStr += name;
    }
    return regexStr;
}

std::vector<std::string> generateQualifiedBannedNames(
    const std::vector<std::string>& polyfillRequiredNames) {
    std::vector<std::string> qualifiedNames;
    for (const auto& name : polyfillRequiredNames) {
        qualifiedNames.push_back("std::" + name);
        qualifiedNames.push_back("boost::" + name);
    }
    return qualifiedNames;
}

MongoPolyFillCheck::MongoPolyFillCheck(StringRef Name, clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {
    // List of banned names from the std and boost namespaces to be checked
    std::vector<std::string> polyfillRequiredNames = {
        "adopt_lock",
        "async",
        "chrono",
        "condition_variable",
        "condition_variable_any",
        "cv_status",
        "defer_lock",
        "future",
        "future_status",
        "get_terminate",
        "launch",
        "lock_guard",
        "mutex",
        "notify_all_at_thread_exit",
        "packaged_task",
        "promise",
        "recursive_mutex",
        "set_terminate",
        "shared_lock",
        "shared_mutex",
        "shared_timed_mutex",
        "this_thread",
        "thread",
        "timed_mutex",
        "try_to_lock",
        "unique_lock",
        "unordered_map",
        "unordered_multimap",
        "unordered_multiset",
        "unordered_set",
    };
    bannedNamesPattern = generateBannedNamesPattern(polyfillRequiredNames);
    qualifiedBannedNames = generateQualifiedBannedNames(polyfillRequiredNames);
}


void MongoPolyFillCheck::registerMatchers(ast_matchers::MatchFinder* Finder) {
    // Matcher to find usage of specified std or boost types
    Finder->addMatcher(loc(hasUnqualifiedDesugaredType(recordType(hasDeclaration(
                               namedDecl(matchesName("(" + bannedNamesPattern + ")$"))))))
                           .bind("stdBoostType"),
                       this);
}

void MongoPolyFillCheck::check(const ast_matchers::MatchFinder::MatchResult& Result) {

    const auto* MatchedTypeLoc = Result.Nodes.getNodeAs<TypeLoc>("stdBoostType");
    if (MatchedTypeLoc) {
        auto typeStr = MatchedTypeLoc->getType().getAsString();
        // we catch this_thread but not this_thread::at_thread_exit
        if (typeStr.find("this_thread::at_thread_exit") != std::string::npos)
            return;
        for (const auto& name : qualifiedBannedNames) {
            if ((typeStr.find("std") == 0 || typeStr.find("boost") == 0) &&
                typeStr.find(name) != std::string::npos) {
                auto location = MatchedTypeLoc->getBeginLoc();
                if (location.isValid())
                    // Found a match
                    diag(MatchedTypeLoc->getBeginLoc(),
                         "Illegal use of banned name from std::/boost:: for %0, use mongo::stdx:: "
                         "variant instead")
                        << typeStr;
            }
        }
    }
}
}  // namespace mongo::tidy
