/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonobj_comparator_interface.h"

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
