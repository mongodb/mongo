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

#pragma once

#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <boost/container_hash/hash.hpp>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDOUT_FILENO _fileno(stdout)
#else
#include <unistd.h>
#endif

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"

namespace mongo::query_tester {
static constexpr auto kFeatureExtractorDir = "/home/ubuntu/feature-extractor"_sd;
static constexpr auto kShellMaxLen = std::numeric_limits<size_t>::max();
static constexpr Milliseconds kShellTimeout = Hours{1};

enum class DiffStyle { kPlain, kWord };
enum class ErrorLogLevel { kSimple, kVerbose, kExtractFeatures };
enum class WriteOutOptions { kNone, kResult, kOnelineResult };

struct CollectionSpec {
    // Implements spaceship-compare for CollectionSpec, which implicitly also implements the
    // operators <, <=, >, >=, ==, and !=.
    auto operator<=>(const CollectionSpec& other) const {
        if (const auto compare = (collName <=> other.collName); !std::is_eq(compare)) {
            return compare;
        } else {
            return filePath <=> other.filePath;
        }
    }

    const std::string collName;
    const std::filesystem::path filePath;
    const std::string rawString;
};

inline bool isTerminal() {
    static const bool isTerminal = isatty(STDOUT_FILENO) != 0;
    return isTerminal;
}

class ConditionalColor {
public:
    explicit ConditionalColor(StringData colorCode) : _colorCode(colorCode) {}

    friend std::ostream& operator<<(std::ostream& os, const ConditionalColor& cc) {
        if (isTerminal()) {
            os << cc._colorCode;
        }
        return os;
    }

private:
    StringData _colorCode;
};

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
        if constexpr (std::is_same_v<T, BSONObj> || std::is_same_v<T, BSONElement>) {
            stream << it->jsonString(ExtendedRelaxedV2_0_0, false, false);
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

ConditionalColor applyBold();
ConditionalColor applyBrown();
ConditionalColor applyCyan();
ConditionalColor applyRed();
ConditionalColor applyReset();
ConditionalColor applyYellow();

StatusWith<std::string> executeShellCmd(const std::string& cmd);

std::string getBaseNameFromFilePath(const std::filesystem::path&);
/**
 * Extracts the test numbers associated with failing queries from hunk headers in the git diff
 * output and stores them in a set for deduplication.
 */
std::set<size_t> getFailedTestNums(const std::string& diffOutput);
std::string getMongoRepoRoot();
/**
 * Performs a text-based diff between the expected and actual result test files and returns the diff
 * output.
 */
std::string gitDiff(const std::filesystem::path&, const std::filesystem::path&, DiffStyle);

bool matchesPrefix(const std::string& key);
void printFailureSummary(const std::vector<std::filesystem::path>& failedTestFiles,
                         size_t failedQueryCount,
                         size_t totalTestsRun);
std::vector<std::string> readAndAssertNewline(std::fstream&, const std::string& context);
std::vector<std::string> readLine(std::fstream&, std::string& lineFromFile);
/**
 * A feature is of the format <FeatureCategory>:<Feature> (ex: Operator:$accumulator) or
 * <FeatureCategory>.<Feature> (ex: IndexProperty.isUnique). This function splits the feature
 * struture on either a ":" or "." delimiter depending on its format.
 */
std::pair<std::string, std::string> splitFeature(const std::string& feature);
DiffStyle stringToDiffStyle(const std::string&);
WriteOutOptions stringToWriteOutOpt(const std::string& opt);
CollectionSpec toCollectionSpec(const std::string&);
void verifyFileStreamGood(std::fstream&, const std::filesystem::path&, const std::string& op);
}  // namespace mongo::query_tester

template <>
struct std::hash<mongo::query_tester::CollectionSpec> {
    std::size_t operator()(const mongo::query_tester::CollectionSpec& spec) const noexcept {
        auto value = size_t{0};
        boost::hash_combine(value, spec.collName);
        boost::hash_combine(value, spec.filePath);
        return value;
    }
};
