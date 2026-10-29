// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
