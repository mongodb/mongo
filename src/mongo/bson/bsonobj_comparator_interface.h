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

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonobj.h"

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
class BSONObj::ComparatorInterface : public BSONComparatorInterfaceBase<BSONObj> {
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
