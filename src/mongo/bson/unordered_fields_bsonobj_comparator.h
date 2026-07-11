// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A BSONObj comparator that supports unordered element comparison.
 */
class UnorderedFieldsBSONObjComparator final : public BSONObj::ComparatorInterface {
public:
    UnorderedFieldsBSONObjComparator(const StringDataComparator* stringComparator = nullptr)
        : _stringComparator(stringComparator) {}

    int compare(const BSONObj& lhs, const BSONObj& rhs) const final {
        return lhs.woCompare(rhs,
                             BSONObj(),
                             ComparisonRules::kIgnoreFieldOrder |
                                 ComparisonRules::kConsiderFieldName,
                             _stringComparator);
    }

    void hash_combine(size_t& seed, const BSONObj& toHash) const final {
        hashCombineBSONObj(seed,
                           toHash,
                           ComparisonRules::kIgnoreFieldOrder | ComparisonRules::kConsiderFieldName,
                           _stringComparator);
    }

private:
    const StringDataComparator* _stringComparator;
};
}  // namespace mongo
