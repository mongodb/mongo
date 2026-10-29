// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A BSONElement comparator that supports:
 * - Ignoring field names during comparison.
 * - Passing a custom string comparator.
 */
class BSONElementComparator final : public BSONElement::ComparatorInterface {
public:
    enum class FieldNamesMode {
        kConsider,
        kIgnore,
    };

    /**
     * Constructs a BSONElement comparator.
     *
     * Will not consider the elements' field names in comparisons if 'fieldNamesMode' is kIgnore.
     *
     * If 'stringComparator' is null, uses default binary string comparison. Otherwise,
     * 'stringComparator' is used for all string comparisons.
     */
    BSONElementComparator(FieldNamesMode fieldNamesMode,
                          const StringDataComparator* stringComparator)
        : _stringComparator(stringComparator),
          _rules((fieldNamesMode == FieldNamesMode::kConsider) ? ComparisonRules::kConsiderFieldName
                                                               : 0) {}

    int compare(const BSONElement& lhs, const BSONElement& rhs) const final {
        return lhs.woCompare(rhs, _rules, _stringComparator);
    }

    void hash_combine(size_t& seed, const BSONElement& toHash) const final {
        hashCombineBSONElement(seed, toHash, _rules, _stringComparator);
    }

private:
    const StringDataComparator* _stringComparator;
    ComparisonRulesSet _rules;
};

}  // namespace mongo
