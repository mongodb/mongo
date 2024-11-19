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

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDOUT_FILENO _fileno(stdout)
#else
#include <unistd.h>
#endif

#include "mongo/bson/bsonelement.h"

namespace queryTester {
enum class WriteOutOptions { kNone, kResult, kOnelineResult };
WriteOutOptions stringToWriteOutOpt(const std::string& opt);

inline bool isTerminal() {
    static const bool isTerminal = isatty(STDOUT_FILENO) != 0;
    return isTerminal;
}

namespace ColorCodes {
constexpr std::string_view kRed = "\033[1;31m";
constexpr std::string_view kYellow = "\033[1;33m";
constexpr std::string_view kCyan = "\033[1;36m";
constexpr std::string_view kBrown = "\033[33m";
constexpr std::string_view kBold = "\033[1m";
constexpr std::string_view kReset = "\033[m";
}  // namespace ColorCodes

class ConditionalColor {
public:
    explicit ConditionalColor(std::string_view colorCode) : _colorCode(colorCode) {}

    friend std::ostream& operator<<(std::ostream& os, const ConditionalColor& cc) {
        if (isTerminal()) {
            os << cc._colorCode;
        }
        return os;
    }

private:
    std::string_view _colorCode;
};

inline ConditionalColor applyRed() {
    return ConditionalColor(ColorCodes::kRed);
}
inline ConditionalColor applyYellow() {
    return ConditionalColor(ColorCodes::kYellow);
}
inline ConditionalColor applyCyan() {
    return ConditionalColor(ColorCodes::kCyan);
}
inline ConditionalColor applyBrown() {
    return ConditionalColor(ColorCodes::kBrown);
}
inline ConditionalColor applyBold() {
    return ConditionalColor(ColorCodes::kBold);
}
inline ConditionalColor applyReset() {
    return ConditionalColor(ColorCodes::kReset);
}

namespace fileHelpers {
// Returns a {collName, fileName} tuple.
std::tuple<std::string, std::filesystem::path> getCollAndFileName(const std::string&);
/**
 * Extracts the test numbers associated with failing queries from hunk headers in the git diff
 * output and stores them in a vector.
 */
std::vector<size_t> getFailedTestNums(const std::string& diffOutput);
std::string getTestNameFromFilePath(const std::filesystem::path&);
/**
 * Performs a text-based diff between the expected and actual result test files and returns the diff
 * output.
 */
std::string gitDiff(const std::filesystem::path&, const std::filesystem::path&);
void printFailureSummary(const std::vector<std::filesystem::path>& failedTestFiles,
                         size_t failedQueryCount,
                         size_t totalTestsRun);
std::vector<std::string> readAndAssertNewline(std::fstream&, const std::string& context);
std::vector<std::string> readLine(std::fstream&, std::string& lineFromFile);
void verifyFileStreamGood(std::fstream&, const std::filesystem::path&, const std::string& op);

template <typename T, bool Multiline>
class TestResult {
public:
    explicit TestResult(const std::vector<T>& value) : _value(value) {}

    const std::vector<T>& get() const {
        return _value;
    }

private:
    const std::vector<T>& _value;
};

template <typename T, bool Multiline>
inline std::ostream& operator<<(std::ostream& stream, const TestResult<T, Multiline>& result) {
    stream << "[";
    const auto& arr = result.get();
    auto start = arr.begin();
    for (auto it = start; it != arr.end(); it++) {
        stream << (it == start ? "" : ",");
        if constexpr (Multiline) {
            stream << std::endl;
        } else {
            stream << " ";
        }
        if constexpr (std::is_same_v<T, mongo::BSONObj> || std::is_same_v<T, mongo::BSONElement>) {
            stream << it->jsonString(mongo::ExtendedRelaxedV2_0_0, false, false);
        } else {
            stream << *it;
        }
    }
    if constexpr (Multiline) {
        stream << std::endl;
    }
    stream << "]" << std::endl;
    return stream;
}

template <typename T>
using ArrayResult = TestResult<T, true>;
template <typename T>
using LineResult = TestResult<T, false>;
}  // namespace fileHelpers
}  // namespace queryTester
