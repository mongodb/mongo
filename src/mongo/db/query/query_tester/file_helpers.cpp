/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "file_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/pcre.h"
#include "mongo/util/shell_exec.h"
#include "mongo/util/str.h"

#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>

namespace mongo::query_tester {
namespace {
constexpr auto kColorBold = "\033[1m"_sd;
constexpr auto kColorBrown = "\033[33m"_sd;
constexpr auto kColorCyan = "\033[1;36m"_sd;
constexpr auto kColorRed = "\033[1;31m"_sd;
constexpr auto kColorReset = "\033[m"_sd;
constexpr auto kColorYellow = "\033[1;33m"_sd;

/**
 * Regex to match the hunk header of the git diff output, which looks like @@ -lineNum, +lineNum @@
 * <Test Number>. We check that the line contains "@@", followed by optional ANSII escape sequences
 * for terminal color formatting, whitespace, and a number representing the test number at the end.
 * The test number will be captured as part of the summary of failing queries.
 */
static const auto kTestNumRegex =
    pcre::Regex{R"(@@(?:\x1B\[[0-9;]*m)*[[:space:]]+(?:\x1B\[[0-9;]*m)*([0-9]+))"};

/**
 * Vector of relevant feature types to extract and display from a set of pipelines. These feature
 * types represent the prefix of an entire feature, which is of the form
 * <feature_type>:<feature_parameter>. There are more feature types listed in
 * https://github.com/10gen/feature-extractor, but the ones that we care about are either Operator,
 * Pipeline, Plan, or Index features.
 */
static const auto kFeatureTypes = std::vector<std::string>{/* Operator Features */
                                                           "ConstantOperator",
                                                           "Operator",

                                                           /* Pipeline Features */
                                                           "MatchTopOperator",
                                                           "PipelineFirstStage",
                                                           "PipelineStage",
                                                           // PipelineStage
                                                           // PipelineStageConstantArgument
                                                           // PipelineStageRelationship
                                                           "PipelineVariableReference",

                                                           /* Plan Features */
                                                           "PlanStage",
                                                           // PlanStageRelationship
                                                           // PlanStageQuality
                                                           "PlannerProperty",
                                                           "RejectedPlan",
                                                           // RejectedPlans
                                                           // RejectedPlanCount
                                                           "SBEStage",
                                                           "SlotBasedPlan",

                                                           /* Lookup Features */
                                                           "Lookup",
                                                           // LookupStrategy
                                                           // LookupIndexesUsed

                                                           /* Index Features */
                                                           "IndexProperty",

                                                           /* Errors */
                                                           "AnyError",
                                                           "SpecificError"};

// Discover the MongoDB repository root with git command.
std::string discoverMongoRepoRoot() {
    const auto gitCmd = std::string{"git rev-parse --show-toplevel"};
    const auto res = executeShellCmd(gitCmd);
    if (!res.isOK()) {
        uasserted(9699502,
                  "Error: Unable to execute git command. Ensure this is run from within the mongo "
                  "git repo.");
    }
    auto repoRoot = res.getValue();
    // Trim trailing whitespace.
    boost::algorithm::trim_right(repoRoot);
    return repoRoot;
}
}  // namespace

ConditionalColor applyBold() {
    return ConditionalColor(kColorBold);
}

ConditionalColor applyBrown() {
    return ConditionalColor(kColorBrown);
}

ConditionalColor applyCyan() {
    return ConditionalColor(kColorCyan);
}

ConditionalColor applyRed() {
    return ConditionalColor(kColorRed);
}

ConditionalColor applyReset() {
    return ConditionalColor(kColorReset);
}

ConditionalColor applyYellow() {
    return ConditionalColor(kColorYellow);
}

StatusWith<std::string> executeShellCmd(const std::string& cmd) {
    return shellExec(cmd, kShellTimeout, kShellMaxLen, true);
}

std::string getBaseNameFromFilePath(const std::filesystem::path& filePath) {
    return filePath.stem().string();
}

std::set<size_t> getFailedTestNums(const std::string& diffOutput) {
    auto failedTestNums = std::set<size_t>{};
    auto line = std::string{};
    auto diffStream = std::istringstream{diffOutput};

    while (std::getline(diffStream, line)) {
        if (auto match = kTestNumRegex.match(line)) {
            failedTestNums.insert(std::stoull(std::string(match[1])));
        }
    }
    return failedTestNums;
}

std::string getMongoRepoRoot() {
    // Cache the discovered repo root value statically.
    static const auto repoRoot = discoverMongoRepoRoot();
    return repoRoot;
}

std::string gitDiff(const std::filesystem::path& expected,
                    const std::filesystem::path& actual,
                    const DiffStyle diffStyle) {
    const auto gitDiffCmd =
        (std::stringstream{}
         << "git"
         // Enrich the hunk header with the test number associated with the diff. The config option
         // override allows us to specify a custom regex function with which to replace the hunk
         // header text in "@@ lines changed @@ TEXT." Files with the .actual and .results
         // extensions will use this custom function (set in the .gitattributes file).
         << " -c diff.query.xfuncname=\"^[0-9]+$\""
         << " diff"
         // The --no-index option allows us to compare files that are not in any repository. -U0
         // removes any lines of context around the diff so that the correct test number directly
         // preceding the diff will be captured.
         << " --no-index "
         << (diffStyle == DiffStyle::kWord ? "--word-diff=color" : "--no-color")
         // Use character-based-diff when in non-CI mode for (hopefully) clearer diffs.
         << (diffStyle == DiffStyle::kPlain ? "" : " --word-diff-regex=.") << " -U0 -- " << expected
         << " " << actual << " 2>&1")
            .str();

    // Need to ignore exit status because the implied --exit-code will return an error sttatus when
    // there is a diff.
    if (auto result = executeShellCmd(gitDiffCmd); result.isOK()) {
        return result.getValue();
    } else {
        return std::string{};
    }
}

bool matchesPrefix(const std::string& key) {
    // Check if the key starts with any of the valid prefixes.
    return std::any_of(kFeatureTypes.begin(),
                       kFeatureTypes.end(),
                       [&key](const std::string& prefix) { return key.starts_with(prefix); });
}

void printFailureSummary(const std::vector<std::filesystem::path>& failedTestFiles,
                         const size_t failedQueryCount,
                         const size_t totalTestsRun) {
    std::cout << applyRed()
              << "============================================================" << applyReset()
              << std::endl
              << applyYellow() << "                       FAILURE SUMMARY                      "
              << applyReset() << std::endl
              << applyRed()
              << "============================================================" << applyReset()
              << std::endl
              << applyCyan() << "SUMMARY: " << applyReset() << std::endl
              << "  Failed test files: " << failedTestFiles.size() << std::endl
              << "  Failed queries: " << failedQueryCount << "/" << totalTestsRun << std::endl
              << std::endl
              << applyRed() << "Failures:" << applyReset() << std::endl;

    for (const auto& failedTestName : failedTestFiles) {
        std::cout << failedTestName << std::endl;
    }

    std::cout << applyRed()
              << "============================================================" << applyReset()
              << std::endl;
}

std::vector<std::string> readAndAssertNewline(std::fstream& fs, const std::string& context) {
    auto lineFromFile = std::string{};
    auto comments = readLine(fs, lineFromFile);
    uassert(9670410,
            str::stream{} << "Expected newline in context '" << context << "' but got "
                          << lineFromFile,
            lineFromFile.empty());
    return comments;
}

std::vector<std::string> readLine(std::fstream& fs, std::string& lineFromFile) {
    auto comments = std::vector<std::string>{};
    std::getline(fs, lineFromFile);
    while (lineFromFile.starts_with("//")) {
        comments.push_back(lineFromFile);
        std::getline(fs, lineFromFile);
    }
    return comments;
}

std::pair<std::string, std::string> splitFeature(const std::string& feature) {
    const auto colonPos = feature.find(':');
    const auto dotPos = feature.find('.');

    // Use the first delimiter (smallest non-npos value between colonPos and dotPos).
    const auto delimiterPos =
        (colonPos != std::string::npos && (dotPos == std::string::npos || colonPos < dotPos))
        ? colonPos
        : dotPos;

    if (delimiterPos != std::string::npos) {
        return {feature.substr(0, delimiterPos), feature.substr(delimiterPos + 1)};
    } else {
        return {feature, ""};
    }
}

DiffStyle stringToDiffStyle(const std::string& style) {
    static const auto kStringToDiffStyleMap =
        std::map<std::string, DiffStyle>{{"plain", DiffStyle::kPlain}, {"word", DiffStyle::kWord}};

    if (auto it = kStringToDiffStyleMap.find(style); it != kStringToDiffStyleMap.end()) {
        return it->second;
    } else {
        uasserted(9764301, str::stream{} << "Unexpected diff style " << style);
    }
}

WriteOutOptions stringToWriteOutOpt(const std::string& opt) {
    static const auto kStringToWriteOutOptMap = std::map<std::string, WriteOutOptions>{
        {"result", WriteOutOptions::kResult}, {"oneline", WriteOutOptions::kOnelineResult}};

    if (auto it = kStringToWriteOutOptMap.find(opt); it != kStringToWriteOutOptMap.end()) {
        return it->second;
    } else {
        uasserted(9670453, str::stream{} << "Unexpected write opt " << opt);
    }
}

CollectionSpec toCollectionSpec(const std::string& collSpecString) {
    if (auto ss = std::stringstream{collSpecString}; !ss.eof()) {
        auto filePath = std::string{};
        ss >> filePath;
        uassert(9670429,
                str::stream{} << "Expected collection file name to end in .coll, but it is "
                              << filePath,
                filePath.ends_with(".coll"));
        if (!ss.eof()) {
            auto token = std::string{};
            ss >> token;
            uassert(9670430,
                    str::stream{} << "Expected token 'as' after collection name, but got " << token,
                    token == "as");
            // The aliased collection name is read into collName using the "as" syntax.
            auto collName = std::string{};
            ss >> collName;
            return {collName, filePath, collSpecString};
        } else {
            return {getBaseNameFromFilePath(filePath), filePath, collSpecString};
        }
    } else {
        uassert(9670431, str::stream{} << "Unexpected empty line.", !ss.eof());
        MONGO_UNREACHABLE;
    }
}

void verifyFileStreamGood(std::fstream& fs,
                          const std::filesystem::path& nameString,
                          const std::string& op) {
    uassert(9670454,
            str::stream{} << "Error while operating on " << nameString.string() << " with error \" "
                          << strerror(errno) << "\": " << op,
            fs.good() || fs.eof());
}
}  // namespace mongo::query_tester
