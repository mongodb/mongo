// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A BSONObj comparator that supports:
 * - Comparing with respect to an ordering spec such as {a: 1, b: -1}.
 * - Ignoring field names during comparison.
 * - Passing a custom string comparator.
 */
class BSONObjComparator final : public BSONObj::ComparatorInterface {
public:
    enum class FieldNamesMode {
        kConsider,
        kIgnore,
    };

    /**
     * Constructs a BSONObj comparator which will use the 'ordering' pattern for comparisons.
     *
     * Will not consider BSON field names in comparisons if 'fieldNamesMode' is kIgnore.
     *
     * If 'stringComparator' is null, uses default binary string comparison. Otherwise,
     * 'stringComparator' is used for all string comparisons.
     */
    BSONObjComparator(BSONObj ordering,
                      FieldNamesMode fieldNamesMode,
                      const StringDataComparator* stringComparator)
        : _ordering(std::move(ordering)),
          _stringComparator(stringComparator),
          _rules((fieldNamesMode == FieldNamesMode::kConsider) ? ComparisonRules::kConsiderFieldName
                                                               : 0) {}

    int compare(const BSONObj& lhs, const BSONObj& rhs) const final {
        return lhs.woCompare(rhs, _ordering, _rules, _stringComparator);
    }

    void hash_combine(size_t& seed, const BSONObj& toHash) const final {
        hashCombineBSONObj(seed, toHash, _rules, _stringComparator);
    }

private:
    BSONObj _ordering;
    const StringDataComparator* _stringComparator;
    ComparisonRulesSet _rules;
};

}  // namespace mongo
