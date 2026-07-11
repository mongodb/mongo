// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
