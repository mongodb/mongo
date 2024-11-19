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

#include <fstream>
#include <ostream>
#include <regex>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/util/shell_exec.h"
#include "mongo/util/str.h"

namespace queryTester {
namespace {
/**
 * Regex to match the hunk header of the git diff output, which looks like @@ -lineNum, +lineNum @@
 * <Test Number>. We check that the line contains "@@", followed by optional ANSII escape sequences
 * for terminal color formatting, whitespace, and a number representing the test number at the end.
 * The test number will be captured as part of the summary of failing queries.
 */
static const auto TEST_NUM_REGEX =
    std::regex{R"(@@(?:\x1B\[[0-9;]*m)*[[:space:]]+(?:\x1B\[[0-9;]*m)*([0-9]+))"};
}  // namespace

WriteOutOptions stringToWriteOutOpt(const std::string& opt) {
    static const auto kStringToWriteOutOptMap = std::map<std::string, WriteOutOptions>{
        {"result", WriteOutOptions::kResult}, {"oneline", WriteOutOptions::kOnelineResult}};

    if (auto it = kStringToWriteOutOptMap.find(opt); it != kStringToWriteOutOptMap.end()) {
        return it->second;
    } else {
        uasserted(9670453, mongo::str::stream{} << "Unexpected write opt " << opt);
    }
}

// Returns a {collName, fileName} tuple.
std::tuple<std::string, std::filesystem::path> fileHelpers::getCollAndFileName(
    const std::string& collSpec) {
    if (auto ss = std::stringstream{collSpec}; !ss.eof()) {
        auto filePath = std::string{};
        ss >> filePath;
        uassert(9670429,
                mongo::str::stream{} << "Expected collection file name to end in .coll, but it is "
                                     << filePath,
                filePath.ends_with(".coll"));
        if (!ss.eof()) {
            auto token = std::string{};
            ss >> token;
            uassert(9670430,
                    mongo::str::stream{} << "Expected token 'as' after collection name, but got "
                                         << token,
                    token == "as");
            auto collName = std::string{};
            ss >> collName;
            return {collName, filePath};
        } else {
            return {
                getTestNameFromFilePath(filePath),
                filePath,
            };
        }
    } else {
        uassert(9670431, mongo::str::stream{} << "Unexpected empty line.", !ss.eof());
        MONGO_UNREACHABLE;
    }
}

std::vector<size_t> fileHelpers::getFailedTestNums(const std::string& diffOutput) {
    auto failedTestNums = std::vector<size_t>{};
    auto line = std::string{};
    auto diffStream = std::istringstream{diffOutput};

    while (std::getline(diffStream, line)) {
        if (auto match = std::smatch{}; std::regex_search(line, match, TEST_NUM_REGEX)) {
            failedTestNums.push_back(std::stoull(match[1]));
        }
    }
    return failedTestNums;
}

std::string fileHelpers::getTestNameFromFilePath(const std::filesystem::path& filePath) {
    auto fileName = filePath.filename().string();
    auto extension = fileName.find('.');
    return fileName.substr(0, extension);
}

std::string fileHelpers::gitDiff(const std::filesystem::path& expected,
                                 const std::filesystem::path& actual) {
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
         << " --no-index --word-diff=color -U0 -- " << expected << " " << actual << " 2>&1")
            .str();
    static constexpr auto kTimeout = mongo::Milliseconds{60 * 60 * 1000};  // 1 hour.

    // Need to ignore exit status because the implied --exit-code will return an error sttatus when
    // there is a diff.
    if (auto result = mongo::shellExec(gitDiffCmd, kTimeout, 1ULL << 32, true); result.isOK()) {
        return result.getValue();
    } else {
        return std::string{};
    }
}

void fileHelpers::printFailureSummary(const std::vector<std::filesystem::path>& failedTestFiles,
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

std::vector<std::string> fileHelpers::readAndAssertNewline(std::fstream& fs,
                                                           const std::string& context) {
    auto lineFromFile = std::string{};
    auto comments = readLine(fs, lineFromFile);
    uassert(9670410,
            mongo::str::stream{} << "Expected newline in context '" << context << "' but got "
                                 << lineFromFile,
            lineFromFile.empty());
    return comments;
}

std::vector<std::string> fileHelpers::readLine(std::fstream& fs, std::string& lineFromFile) {
    auto comments = std::vector<std::string>{};
    std::getline(fs, lineFromFile);
    while (lineFromFile.starts_with("//")) {
        comments.push_back(lineFromFile);
        std::getline(fs, lineFromFile);
    }
    return comments;
}

void fileHelpers::verifyFileStreamGood(std::fstream& fs,
                                       const std::filesystem::path& nameString,
                                       const std::string& op) {
    uassert(9670454,
            mongo::str::stream{} << "Error while operating on " << nameString.string()
                                 << " with error \" " << strerror(errno) << "\": " << op,
            fs.good() || fs.eof());
}
}  // namespace queryTester
