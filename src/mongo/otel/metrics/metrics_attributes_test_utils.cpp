// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/otel/metrics/metrics_attributes_test_utils.h"

namespace mongo::otel::metrics {
bool attributeValueEquals(const AnyAttributeType& lhs, const AnyAttributeType& rhs) {
    if (lhs.index() != rhs.index())
        return false;
    return std::visit(
        [&rhs](const auto& lhsVal) -> bool {
            using T = std::decay_t<decltype(lhsVal)>;
            const auto& rhsVal = std::get<T>(rhs);
            return attributeValuesEqual(lhsVal, rhsVal);
        },
        lhs);
}

bool operator==(const AttributeNameAndValue& lhs, const AttributeNameAndValue& rhs) {
    return lhs.name == rhs.name && attributeValueEquals(lhs.value, rhs.value);
}
}  // namespace mongo::otel::metrics
