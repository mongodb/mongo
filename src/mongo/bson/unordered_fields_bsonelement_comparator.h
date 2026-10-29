// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A BSONElement comparator that supports unordered element comparisons for objects. Does not
 * support using a non-simple string comparator.
 */
class UnorderedFieldsBSONElementComparator final : public BSONElement::ComparatorInterface {
public:
    static constexpr StringDataComparator* kStringComparator = nullptr;

    UnorderedFieldsBSONElementComparator() = default;

    int compare(const BSONElement& lhs, const BSONElement& rhs) const final {
        return lhs.woCompare(rhs, ComparisonRules::kIgnoreFieldOrder, kStringComparator);
    }

    void hash_combine(size_t& seed, const BSONElement& toHash) const final {
        hashCombineBSONElement(seed, toHash, ComparisonRules::kIgnoreFieldOrder, kStringComparator);
    }
};

}  // namespace mongo
