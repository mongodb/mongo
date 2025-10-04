/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str_escape.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>  // IWYU pragma: keep
#include <iostream>
#include <map>
#include <memory>
#include <utility>

namespace mongo::unittest {
namespace {
static constexpr const char* kTempFileSuffix = ".tmp.txt";

// Map from file name to a list of updates. We keep track of how many lines are added or deleted at
// a particular line of a source file.
using LineDeltaVector = std::vector<std::pair<uint64_t, int64_t>>;
std::map<std::string, LineDeltaVector> gLineDeltaMap;

// Different compilers expand __LINE__ differently. For multi-line macros, newer versions of GCC use
// the first line, while older versions use the last line. This flag accounts for the difference in
// behavior.
#if defined(__GNUC__) && (__GNUC__) >= 11
static constexpr bool kIsFirstLine = true;
#else
static constexpr bool kIsFirstLine = false;
#endif

std::vector<std::string> formatStr(const std::string& str, const bool needsEscaping) {
    std::vector<std::string> replacementLines;
    const bool endsWithNewline = str.back() == '\n';

    std::vector<std::string> inputLines;
    std::istringstream inputStream(str);
    std::string line;
    while (std::getline(inputStream, line)) {
        inputLines.push_back(std::move(line));
    }
    for (size_t i = 0; i < inputLines.size(); i++) {
        // Process the string line by line and format it to match the test file's expected format.
        // We have an initial indentation, followed by quotes and the escaped string itself.
        std::string escaped =
            needsEscaping ? mongo::str::escapeForJSON(inputLines[i]) : inputLines[i];
        for (;;) {
            // If the line is estimated to exceed the maximum length allowed by the linter, break it
            // up and make sure to insert '\n' only at the end of the last segment.
            const bool breakupLine = needsEscaping && escaped.size() > kAutoUpdateMaxLineLength;

            size_t lineLength = needsEscaping ? kAutoUpdateMaxLineLength : escaped.size();
            if (breakupLine) {
                // Attempt to break the line at the last space.
                lineLength = escaped.find_last_of(' ', lineLength);
                if (lineLength == std::string::npos) {
                    // Line does not have any spaces.
                    lineLength = kAutoUpdateMaxLineLength;
                } else {
                    lineLength++;
                }
            }

            std::ostringstream os;
            os << "        ";
            if (needsEscaping) {
                os << "\"";
            }

            os << escaped.substr(0, lineLength);

            if (needsEscaping) {
                if (!breakupLine && (endsWithNewline || i + 1 != inputLines.size())) {
                    os << "\\n";
                }
                os << "\"";
            }
            os << "\n";

            replacementLines.push_back(os.str());

            if (breakupLine) {
                escaped = escaped.substr(lineLength);
            } else {
                break;
            }
        }
    }

    if (!replacementLines.empty() && !replacementLines.back().empty()) {
        // Account for the fact that we need an extra comma after the string constant in the macro.
        auto& lastLine = replacementLines.back();
        lastLine.insert(lastLine.size() - 1, ",");
    }

    return replacementLines;
}

boost::optional<size_t> diffLookAhead(const size_t thisIndex,
                                      const std::vector<std::string>& thisSideLines,
                                      const std::string& otherLine) {
    static constexpr size_t kLookAheadSize = 5;

    const size_t maxIndex = std::min(thisIndex + kLookAheadSize, thisSideLines.size());
    for (size_t i = thisIndex + 1; i < maxIndex; i++) {
        if (thisSideLines.at(i) == otherLine) {
            return i;
        }
    }

    return {};
}
}  // namespace

void outputDiff(std::ostream& os,
                const std::vector<std::string>& expFormatted,
                const std::vector<std::string>& actualFormatted,
                const size_t startLineNumber) {
    const size_t actualSize = actualFormatted.size();
    const size_t expSize = expFormatted.size();

    const auto outputLine =
        [&os](const bool isExpected, const size_t lineNumber, const std::string& line) {
            os << "L" << lineNumber << ": " << (isExpected ? "-" : "+") << line;
        };

    size_t expectedIndex = 0;
    size_t actualIndex = 0;
    for (;;) {
        if (actualIndex >= actualSize) {
            // Exhausted actual side.

            if (expectedIndex >= expSize) {
                // Reached the end.
                break;
            }

            // Print remaining expected side.
            const auto& expLine = expFormatted.at(expectedIndex);
            outputLine(true /*isExpected*/, startLineNumber + expectedIndex, expLine);
            expectedIndex++;
            continue;
        }

        const auto& actualLine = actualFormatted.at(actualIndex);
        if (expectedIndex >= expSize) {
            // Exhausted expected side. Print remaining actual side.
            outputLine(false /*isExpected*/, startLineNumber + actualIndex, actualLine);
            actualIndex++;
            continue;
        }

        const auto& expLine = expFormatted.at(expectedIndex);
        if (actualLine == expLine) {
            // Current lines are equal, skip.
            expectedIndex++;
            actualIndex++;
            continue;
        }

        if (const auto fwdIndex = diffLookAhead(actualIndex, actualFormatted, expLine)) {
            // Looked ahead successfully into the actual side. Move actual side forward.
            for (size_t i = actualIndex; i < *fwdIndex; i++) {
                outputLine(false /*isExpected*/, startLineNumber + i, actualFormatted.at(i));
            }
            actualIndex = *fwdIndex;
            continue;
        }
        if (const auto fwdIndex = diffLookAhead(expectedIndex, expFormatted, actualLine)) {
            // Looked ahead successfully into the expected side. Move expected side forward.
            for (size_t i = expectedIndex; i < *fwdIndex; i++) {
                outputLine(true /*isExpected*/, startLineNumber + i, expFormatted.at(i));
            }
            expectedIndex = *fwdIndex;
            continue;
        }

        // Move both sides forward.
        outputLine(false /*isExpected*/, startLineNumber + actualIndex, actualLine);
        outputLine(true /*isExpected*/, startLineNumber + expectedIndex, expLine);
        actualIndex++;
        expectedIndex++;
    }
}

bool handleAutoUpdate(const std::string& expected,
                      const std::string& actual,
                      const std::string& fileName,
                      const size_t lineNumber,
                      const bool needsEscaping) {
    const auto config = mongo::unittest::getAutoUpdateConfig();
    // This function should only be allowed to return early without rewriting the assertion if the
    // rewriteAllAutoAsserts flag is not set.
    const bool canReturnEarly = !config.revalidateAll;
    const bool doesMatch = expected == actual;

    if (canReturnEarly && doesMatch) {
        return true;
    }

    const auto expectedFormatted = formatStr(expected, needsEscaping);
    const auto actualFormatted = formatStr(actual, needsEscaping);

    // Treat an empty string as needing one line. Adjust for line delta.
    const size_t expectedDelta = expectedFormatted.empty() ? 1 : expectedFormatted.size();
    // Compute the macro argument start line.
    const size_t startLineNumber = kIsFirstLine ? (lineNumber + 1) : (lineNumber - expectedDelta);

    // Only show the error message if results differ. To avoid outputting empty diffs when rewriting
    // all assertions.
    if (!doesMatch) {
        std::cout << fileName << ":" << startLineNumber << ": results differ:\n";
        outputDiff(std::cout, expectedFormatted, actualFormatted, startLineNumber);
    }

    if (canReturnEarly && !config.updateFailingAsserts) {
        std::cout << "Auto-updating is disabled.\n";
        return false;
    }

    // If an ...INITIAL_AUTO macro is expanded, the source file is updated with an empty plan just
    // before the call to handleAutoUpdate(). Make an exception and accept more recent source file.
    if (boost::filesystem::last_write_time(boost::filesystem::path(fileName)) >
            boost::filesystem::last_write_time(config.executablePath) &&
        !expectedFormatted.empty()) {
        std::cout << "Source file " << fileName
                  << " was modified more recently than the executable, won't auto-update.\n";
        return false;
    }

    // Compute the total number of lines added or removed before the current macro line.
    auto& lineDeltas = gLineDeltaMap.emplace(fileName, LineDeltaVector{}).first->second;
    int64_t totalDelta = 0;
    for (const auto& [line, delta] : lineDeltas) {
        if (line < startLineNumber) {
            totalDelta += delta;
        }
    }

    const size_t replacementStartLine = startLineNumber + totalDelta;
    const size_t replacementEndLine = replacementStartLine + expectedDelta;

    const std::string tempFileName = fileName + kTempFileSuffix;
    std::string line;
    size_t lineIndex = 0;

    try {
        std::ifstream in;
        in.open(fileName);
        std::ofstream out;
        out.open(tempFileName);

        // Generate a new test file, updated with the replacement string.
        while (std::getline(in, line)) {
            lineIndex++;

            if (lineIndex < replacementStartLine || lineIndex >= replacementEndLine) {
                out << line << "\n";
            } else if (lineIndex == replacementStartLine) {
                for (const auto& line1 : actualFormatted) {
                    out << line1;
                }
            }
        }

        out.close();
        in.close();

        if (std::rename(tempFileName.c_str(), fileName.c_str())) {
            throw std::system_error(lastSystemError());
        }
    } catch (const std::exception& ex) {
        // Print and re-throw exception.
        std::cout << "Caught an exception while manipulating files: " << ex.what();
        throw ex;
    }

    // Add the current delta.
    const int64_t delta = static_cast<int64_t>(actualFormatted.size()) - expectedDelta;
    lineDeltas.emplace_back(startLineNumber, delta);

    // Do not assert in order to allow multiple tests to be updated.
    return true;
}

bool expandNoPlanMacro(const std::string& fileName, const size_t lineNumber) {
    const auto config = mongo::unittest::getAutoUpdateConfig();
    if (!config.updateFailingAsserts) {
        std::cout << "Auto-updating is disabled. To expand the *INITIAL_AUTO macro with"
                     " actual plan, run the test with the flag --autoUpdateAsserts\n";
        return false;
    }

    // Compute the total number of lines added or removed before the current macro line.
    auto& lineDeltas = gLineDeltaMap.emplace(fileName, LineDeltaVector{}).first->second;
    int64_t totalDelta = 0;
    for (const auto& [line, delta] : lineDeltas) {
        if (line < lineNumber) {
            totalDelta += delta;
        }
    }

    const size_t replacementStartLine = lineNumber + totalDelta;
    const size_t replacementEndLine = replacementStartLine + 1;

    const std::string tempFileName = fileName + kTempFileSuffix;
    std::string line;
    size_t lineIndex = 0;

    try {
        std::ifstream in;
        in.open(fileName);
        std::ofstream out;
        out.open(tempFileName);

        // Generate a new test file, updated with the replacement string.
        while (std::getline(in, line)) {
            lineIndex++;

            if (lineIndex < replacementStartLine || lineIndex >= replacementEndLine) {
                out << line << "\n";
            } else if (lineIndex == replacementStartLine) {
                auto pos1 = line.find("_INITIAL");
                auto pos2 = line.find("AUTO(");
                if (pos1 == std::string::npos || pos2 == std::string::npos) {
                    out << line << "\n";
                    std::cout << "The macro doesn't have the expected format. Skip "
                                 "auto-update and keep the original line "
                              << lineIndex << std::endl;
                } else {
                    // Replace
                    // ASSERT_EXPLAIN_INITIAL_AUTO(abt);
                    // with
                    // ASSERT_EXPLAIN_AUTO( // NOLINT
                    //      "",
                    //      abt);
                    out << line.substr(0, pos1) << "_AUTO("
                        << "  // NOLINT\n";
                    out << "        \"\","
                        << "\n";
                    out << "        " << line.substr(pos2 + 5) << "\n";
                }
            }
        }

        out.close();
        in.close();

        if (std::rename(tempFileName.c_str(), fileName.c_str())) {
            throw std::system_error(lastSystemError());
        }
    } catch (const std::exception& ex) {
        // Print and re-throw exception.
        std::cout << "Caught an exception while manipulating files: " << ex.what();
        throw ex;
    }
    return true;
}

void updateDelta(const std::string& fileName, const size_t lineNumber, const int64_t delta) {
    auto& lineDeltas = gLineDeltaMap.emplace(fileName, LineDeltaVector{}).first->second;
    lineDeltas.emplace_back(lineNumber, delta);
}

void expandActualPlan(const SourceLocation& location, const std::string& actual) {
    const auto& fileName = location.file_name();
    const auto& lineNumber = location.line();
    std::cout << "expandActualPlan " << fileName << ":" << lineNumber << std::endl;
    if (::mongo::unittest::expandNoPlanMacro(fileName, lineNumber)) {
        ::mongo::unittest::handleAutoUpdate("", actual, fileName, lineNumber + 2, true);
        // Update the delta with the two lines introduced by expandNoPlanMacro.
        ::mongo::unittest::updateDelta(fileName, lineNumber, 2);
    }
}
}  // namespace mongo::unittest
