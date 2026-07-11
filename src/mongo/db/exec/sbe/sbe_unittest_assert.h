// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"

/**
 * Internal helper for the ASSERT_SBE_VALUE_* macros below; do not use it directly. It cannot be
 * #undef'd here because the wrappers expand it at their point of use.
 */
#define _ASSERT_SBE_VALUE_CMP(...) \
    ::mongo::sbe::value::unittest::assertSbeValueCompare(__FILE__, __LINE__, __VA_ARGS__)

/**
 * Asserts that 'lhs' and 'rhs' satisfy the given comparison relationship under
 * value::compareValue(). Each side may be given either as a single TagValueView or as a separate
 * tag/value pair, so each macro takes either (lhs, rhs) or (lhsTag, lhsVal, rhsTag, rhsVal); the
 * two forms are resolved by ordinary overloading of assertSbeValueCompare(). On failure, the
 * assertion message prints both operands and the expected relationship.
 */
#define ASSERT_SBE_VALUE_EQ(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kEq)
#define ASSERT_SBE_VALUE_NE(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kNe)
#define ASSERT_SBE_VALUE_LT(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kLt)
#define ASSERT_SBE_VALUE_LTE(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kLte)
#define ASSERT_SBE_VALUE_GT(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kGt)
#define ASSERT_SBE_VALUE_GTE(...) \
    _ASSERT_SBE_VALUE_CMP(__VA_ARGS__, ::mongo::sbe::value::unittest::SbeAssertOp::kGte)

namespace mongo::sbe::value::unittest {

/** The comparison relationship asserted between two SBE values by assertSbeValueCompare(). */
enum class SbeAssertOp { kEq, kNe, kLt, kLte, kGt, kGte };

/**
 * Compares 'lhs' and 'rhs' using value::compareValue() and checks that the result satisfies
 * 'op'. On failure, raises a fatal test failure attributed to 'file' and 'line' (the caller's
 * location, captured by the ASSERT_SBE_VALUE_* macros) describing both operands and the
 * expected relationship.
 */
void assertSbeValueCompare(
    const char* file, unsigned line, TagValueView lhs, TagValueView rhs, SbeAssertOp op);

void assertSbeValueCompare(const char* file,
                           unsigned line,
                           TypeTags lhsTag,
                           Value lhsVal,
                           TypeTags rhsTag,
                           Value rhsVal,
                           SbeAssertOp op);

}  // namespace mongo::sbe::value::unittest
