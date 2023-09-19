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
#include <map>
#include <set>

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

namespace mongo {

/**
 * A BSONObj comparator that has simple binary compare semantics.
 */
class SimpleBSONObjComparator final : public BSONObj::ComparatorInterface {
public:
    // Global simple comparator for stateless BSONObj comparisons. BSONObj comparisons that require
    // database logic, such as collations, must instantiate their own comparator.
    static const SimpleBSONObjComparator kInstance;

    int compare(const BSONObj& lhs, const BSONObj& rhs) const final {
        return lhs.woCompare(rhs, BSONObj(), ComparisonRules::kConsiderFieldName, nullptr);
    }

    void hash_combine(size_t& seed, const BSONObj& toHash) const final {
        hashCombineBSONObj(seed, toHash, ComparisonRules::kConsiderFieldName, nullptr);
    }

    /**
     * A functor with simple binary comparison semantics that's suitable for use with ordered STL
     * containers.
     */
    class LessThan {
    public:
        explicit LessThan() = default;

        bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
            return kInstance.compare(lhs, rhs) < 0;
        }
    };

    /**
     * A functor with simple binary comparison semantics that's suitable for use with unordered STL
     * containers.
     */
    class EqualTo {
    public:
        explicit EqualTo() = default;

        bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
            return kInstance.compare(lhs, rhs) == 0;
        }
    };

    /**
     * Functor for computing the hash of a BSONObj, compatible for use with unordered STL
     * containers.
     */
    class Hasher {
    public:
        explicit Hasher() = default;

        size_t operator()(const BSONObj& obj) const {
            return kInstance.hash(obj);
        }
    };
};

inline auto simpleHash(const BSONObj& obj) {
    return SimpleBSONObjComparator::kInstance.hash(obj);
}

/**
 * A set of BSONObjs that performs comparisons with simple binary semantics.
 */
using SimpleBSONObjSet = std::set<BSONObj, SimpleBSONObjComparator::LessThan>;

/**
 * A multiset of BSONObjs that performs comparisons with simple binary semantics.
 */
using SimpleBSONObjMultiSet = std::multiset<BSONObj, SimpleBSONObjComparator::LessThan>;

/**
 * An unordered_set of BSONObjs that performs equality checks using simple binary semantics.
 */
using SimpleBSONObjUnorderedSet =
    stdx::unordered_set<BSONObj, SimpleBSONObjComparator::Hasher, SimpleBSONObjComparator::EqualTo>;

/**
 * A map keyed on BSONObj that performs comparisons with simple binary semantics.
 */
template <typename T>
using SimpleBSONObjMap = std::map<BSONObj, T, SimpleBSONObjComparator::LessThan>;

/**
 * A multimap keyed on BSONObj that performs comparisons with simple binary semantics.
 */
template <typename T>
using SimpleBSONObjMultiMap = std::multimap<BSONObj, T, SimpleBSONObjComparator::LessThan>;

/**
 * An unordered_map keyed on BSONObj that performs equality checks using simple binary semantics.
 */
template <typename T>
using SimpleBSONObjUnorderedMap = stdx::
    unordered_map<BSONObj, T, SimpleBSONObjComparator::Hasher, SimpleBSONObjComparator::EqualTo>;

}  // namespace mongo
