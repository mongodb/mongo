/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data_comparator_interface.h"
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
                      const StringData::ComparatorInterface* stringComparator)
        : _ordering(std::move(ordering)),
          _fieldNamesMode(fieldNamesMode),
          _stringComparator(stringComparator) {}

    int compare(const BSONObj& lhs, const BSONObj& rhs) const final {
        const bool considerFieldName = (_fieldNamesMode == FieldNamesMode::kConsider);
        return lhs.woCompare(rhs, _ordering, considerFieldName, _stringComparator);
    }

    void hash_combine(size_t& seed, const BSONObj& toHash) const final {
        const bool considerFieldName = (_fieldNamesMode == FieldNamesMode::kConsider);
        hashCombineBSONObj(seed, toHash, considerFieldName, _stringComparator);
    }

private:
    BSONObj _ordering;
    FieldNamesMode _fieldNamesMode;
    const StringData::ComparatorInterface* _stringComparator;
};

}  // namespace mongo
