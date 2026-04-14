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
/**
 * Functions and matchers for helping test attribute implementations.
 */
#pragma once

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

/**
 * Returns whether the two AttributeNameAndValue are equal. This is non-trivial due to the fact that
 * the values can be std::spans of different types, which don't support == by default.
 */
bool operator==(const AttributeNameAndValue& lhs, const AttributeNameAndValue& rhs);

/** Allows using matchers on the values and attributes of an AttributesAndValue. */
MATCHER_P2(IsAttributesAndValue, attrsMatcher, valueMatcher, "") {
    return testing::ExplainMatchResult(attrsMatcher, arg.attributes, result_listener) &&
        testing::ExplainMatchResult(valueMatcher, arg.value, result_listener);
}

/**
 * Matches an attribute tuple using AttributesEq, which handles element types like std::span that
 * lack operator==. Use in place of plain equality when matching Attributes.
 *
 * Example:
 *   EXPECT_THAT(map, UnorderedElementsAre(
 *       Pair(IsAttributesTuple(std::make_tuple(std::span<int32_t>(data))), 42)));
 */
template <typename TupleT>
auto IsAttributesTuple(TupleT expected) {
    return testing::Truly(
        [expected](const TupleT& actual) { return AttributesEq{}(actual, expected); });
}
}  // namespace mongo::otel::metrics
