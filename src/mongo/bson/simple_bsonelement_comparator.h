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

#include <cstddef>

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"

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
