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
