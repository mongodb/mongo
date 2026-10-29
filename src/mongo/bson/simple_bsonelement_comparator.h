// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/util/modules.h"

#include <cstddef>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A BSONElement comparator that has simple binary compare semantics. The comparison considers both
 * the field name of the element and the element's value.
 */
class SimpleBSONElementComparator final : public BSONElement::ComparatorInterface {
public:
    // Global simple comparator for stateless BSONObj comparisons. BSONObj comparisons that require
    // database logic, such as collations, much instantiate their own comparator.
    static const SimpleBSONElementComparator kInstance;

    int compare(const BSONElement& lhs, const BSONElement& rhs) const final {
        return lhs.woCompare(rhs, ComparisonRules::kConsiderFieldName, nullptr);
    }

    void hash_combine(size_t& seed, const BSONElement& toHash) const final {
        hashCombineBSONElement(seed, toHash, ComparisonRules::kConsiderFieldName, nullptr);
    }
};

}  // namespace mongo
