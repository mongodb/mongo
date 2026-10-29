// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/container_hash/hash.hpp>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDOUT_FILENO _fileno(stdout)
#else
#include <unistd.h>
#endif

#include "mongo/bson/bsonelement.h"

namespace mongo::query_tester {
using namespace std::literals::string_view_literals;
static constexpr auto kFeatureExtractorDir = "/home/ubuntu/feature-extractor"sv;
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
