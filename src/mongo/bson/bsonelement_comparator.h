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
#include "mongo/bson/bsonelement_comparator_interface.h"

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
                          const StringData::ComparatorInterface* stringComparator)
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
    const StringData::ComparatorInterface* _stringComparator;
    ComparisonRulesSet _rules;
};

}  // namespace mongo
