// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];


namespace mongo {

/**
 * A BSONObj::ComparatorInterface is an abstract class for comparing BSONObj objects. Usage for
 * comparing two BSON objects, 'lhs' and 'rhs', where 'comparator' is an instance of a class
 * implementing this interface, is as shown below:
 *
 *  bool lessThan = comparator.evaluate(lhs < rhs);
 *  bool lessThanOrEqual = comparator.evaluate(lhs <= rhs);
 *  bool equal = comparator.evaluate(lhs == rhs);
 *  bool greaterThanOrEqual = comparator.evaluate(lhs >= rhs);
 *  bool greaterThan = comparator.evaluate(lhs > rhs);
 *  bool notEqual = comparator.evaluate(lhs != rhs);
 *
 * Can also be used to obtain function objects compatible for use with standard library algorithms
 * such as std::sort, and to construct STL sets and maps which respect this comparator.
 *
 * All methods are thread-safe.
 */
class [[MONGO_MOD_OPEN]] BSONObj::ComparatorInterface
    : public BSONComparatorInterfaceBase<BSONObj> {
public:
    /**
     * Constructs a BSONObjSet whose equivalence classes are given by this comparator. This
     * comparator must outlive the returned set.
     */
    Set makeBSONObjSet(std::initializer_list<BSONObj> init = {}) const& {
        return makeSet(init);
    }

    Set makeBSONObjSet(std::initializer_list<BSONObj> init = {}) const&& = delete;

    /**
     * Constructs a BSONObjUnorderedSet whose equivalence classes are given by this
     * comparator. This comparator must outlive the returned set.
     */
    UnorderedSet makeBSONObjUnorderedSet(std::initializer_list<BSONObj> init = {}) const& {
        return makeUnorderedSet(init);
    }

    UnorderedSet makeBSONObjUnorderedSet(std::initializer_list<BSONObj> init = {}) const&& = delete;

    /**
     * Constructs an ordered map from BSONObj to type ValueType whose ordering is given by this
     * comparator.  This comparator must outlive the returned map.
     */
    template <typename ValueType>
    Map<ValueType> makeBSONObjIndexedMap(
        std::initializer_list<std::pair<const BSONObj, ValueType>> init = {}) const& {
        return makeMap(init);
    }

    template <typename ValueType>
    Map<ValueType> makeBSONObjIndexedMap(
        std::initializer_list<std::pair<const BSONObj, ValueType>> init = {}) const&& = delete;

    /**
     * Constructs an unordered map from BSONObj to type ValueType whose ordering is given by this
     * comparator. This comparator must outlive the returned map.
     */
    template <typename ValueType>
    UnorderedMap<ValueType> makeBSONObjIndexedUnorderedMap(
        std::initializer_list<std::pair<const BSONObj, ValueType>> init = {}) const& {
        return makeUnorderedMap(init);
    }

    template <typename ValueType>
    UnorderedMap<ValueType> makeBSONObjIndexedUnorderedMap(
        std::initializer_list<std::pair<const BSONObj, ValueType>> init = {}) const&& = delete;
};

using BSONObjSet = BSONComparatorInterfaceBase<BSONObj>::Set;

using BSONObjUnorderedSet = BSONComparatorInterfaceBase<BSONObj>::UnorderedSet;

template <typename ValueType>
using BSONObjIndexedMap = BSONComparatorInterfaceBase<BSONObj>::Map<ValueType>;

template <typename ValueType>
using BSONObjIndexedUnorderedMap = BSONComparatorInterfaceBase<BSONObj>::UnorderedMap<ValueType>;

}  // namespace mongo
