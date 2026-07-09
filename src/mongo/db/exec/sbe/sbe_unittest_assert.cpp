/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/sbe_unittest_assert.h"

#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/unittest/unittest.h"

#include <sstream>
#include <string_view>

namespace mongo::sbe::value::unittest {
namespace {

/** Returns whether a compareValue() result 'cmp' satisfies the relationship 'op'. */
bool comparisonSatisfiesOp(int32_t cmp, SbeAssertOp op) {
    switch (op) {
        case SbeAssertOp::kEq:
            return cmp == 0;
        case SbeAssertOp::kNe:
            return cmp != 0;
        case SbeAssertOp::kLt:
            return cmp < 0;
        case SbeAssertOp::kLte:
            return cmp <= 0;
        case SbeAssertOp::kGt:
            return cmp > 0;
        case SbeAssertOp::kGte:
            return cmp >= 0;
    }
    MONGO_UNREACHABLE;
}

/** Returns the comparison operator symbol for 'op' (e.g. "==" for kEq) for failure messages. */
std::string_view opToSymbol(SbeAssertOp op) {
    switch (op) {
        case SbeAssertOp::kEq:
            return "==";
        case SbeAssertOp::kNe:
            return "!=";
        case SbeAssertOp::kLt:
            return "<";
        case SbeAssertOp::kLte:
            return "<=";
        case SbeAssertOp::kGt:
            return ">";
        case SbeAssertOp::kGte:
            return ">=";
    }
    MONGO_UNREACHABLE;
}

}  // namespace

void assertSbeValueCompare(
    const char* file, unsigned line, TagValueView lhs, TagValueView rhs, SbeAssertOp op) {
    const auto [cmpTag, cmpVal] = compareValue(lhs.tag, lhs.value, rhs.tag, rhs.value);

    std::ostringstream errorMessage;
    if (cmpTag != TypeTags::NumberInt32) {
        // compareValue() must produce an int32 result for the comparison below to be meaningful;
        // a different tag indicates a bug rather than a test-value mismatch.
        errorMessage << "compareValue returned " << std::make_pair(cmpTag, cmpVal) << " for lhs "
                     << std::make_pair(lhs.tag, lhs.value) << " and rhs "
                     << std::make_pair(rhs.tag, rhs.value);
    } else {
        const auto cmp = bitcastTo<int32_t>(cmpVal);
        if (!comparisonSatisfiesOp(cmp, op)) {
            errorMessage << "expected " << std::make_pair(lhs.tag, lhs.value) << " "
                         << opToSymbol(op) << " " << std::make_pair(rhs.tag, rhs.value)
                         << ", found compare result " << cmp;
        }
    }

    if (!errorMessage.view().empty()) {
        GTEST_MESSAGE_AT_(
            file, line, errorMessage.str().c_str(), ::testing::TestPartResult::kFatalFailure);
    }
}

void assertSbeValueCompare(const char* file,
                           unsigned line,
                           TypeTags lhsTag,
                           Value lhsVal,
                           TypeTags rhsTag,
                           Value rhsVal,
                           SbeAssertOp op) {
    assertSbeValueCompare(
        file, line, TagValueView(lhsTag, lhsVal), TagValueView(rhsTag, rhsVal), op);
}

}  // namespace mongo::sbe::value::unittest
