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

#pragma once

#include <ostream>
#include <vector>

namespace mongo::unittest {
/**
 * Computes a difference between the expected and actual formatted output and outputs it to the
 * provide stream instance. Used to display difference between expected and actual format for
 * auto-update macros. It is exposed in the header here for testability.
 */
void outputDiff(std::ostream& os,
                const std::vector<std::string>& expFormatted,
                const std::vector<std::string>& actualFormatted,
                size_t startLineNumber);

bool handleAutoUpdate(const std::string& expected,
                      const std::string& actual,
                      const std::string& fileName,
                      size_t lineNumber,
                      bool needsEscaping);

// Account for maximum line length after linting. We need to indent, add quotes, etc.
static constexpr size_t kAutoUpdateMaxLineLength = 88;

/**
 * Auto update result back in the source file if the assert fails.
 * The expected result must be a multi-line string in the following form:
 *
 * ASSERT_EXPLAIN_V2_AUTO(     // NOLINT
 *       "BinaryOp [Add]\n"
 *       "|   Const [2]\n"
 *       "Const [1]\n",
 *       tree);
 *
 * Limitations:
 *      1. There should not be any comments or other formatting inside the multi-line string
 *      constant other than 'NOLINT'. If we have a single-line constant, the auto-updating will
 *      generate a 'NOLINT' at the end of the line.
 *      2. The expression which we are explaining ('tree' in the example above) must fit on a single
 *      line.
 *      3. The macro should be indented by 4 spaces.
 */
#define AUTO_UPDATE_HELPER(expected, actual, needsEscaping) \
    ::mongo::unittest::handleAutoUpdate(expected, actual, __FILE__, __LINE__, needsEscaping)

#define ASSERT_STR_EQ_AUTO(expected, actual) ASSERT(AUTO_UPDATE_HELPER(expected, actual, true))

#define ASSERT_NUMBER_EQ_AUTO(expected, actual) \
    ASSERT(AUTO_UPDATE_HELPER(str::stream() << expected, str::stream() << actual, false))
}  // namespace mongo::unittest
