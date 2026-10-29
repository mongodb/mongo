// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <map>
#include <set>

[[MONGO_MOD_PUBLIC]];

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
