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
#include "mongo/bson/bsonelement.h"

namespace mongo {

typedef std::set<BSONElement, BSONElementCmpWithoutField> BSONElementSet;
typedef std::multiset<BSONElement, BSONElementCmpWithoutField> BSONElementMultiSet;

/**
 * A BSONElement::ComparatorInterface is an abstract class for comparing BSONElement objects. Usage
 * for comparing two BSON elements, 'lhs' and 'rhs', where 'comparator' is an instance of a class
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
class BSONElement::ComparatorInterface : public BSONComparatorInterfaceBase<BSONElement> {
public:
    /**
     * Constructs a BSONEltSet whose equivalence classes are given by this comparator. This
     * comparator must outlive the returned set.
     */
    Set makeBSONEltSet(std::initializer_list<BSONElement> init = {}) const& {
        return makeSet(init);
    }

    Set makeBSONEltSet(std::initializer_list<BSONElement> init = {}) const&& = delete;

    /**
     * Constructs a BSONEltUnorderedSet whose equivalence classes are given by this
     * comparator. This comparator must outlive the returned set.
     */
    UnorderedSet makeBSONEltUnorderedSet(std::initializer_list<BSONElement> init = {}) const& {
        return makeUnorderedSet(init);
    }

    UnorderedSet makeBSONEltUnorderedSet(std::initializer_list<BSONElement> init = {}) const&& =
        delete;

    /**
     * Constructs an ordered map from BSONElement to type ValueType whose ordering is given by this
     * comparator. This comparator must outlive the returned map.
     */
    template <typename ValueType>
    Map<ValueType> makeBSONEltIndexedMap(
        std::initializer_list<std::pair<const BSONElement, ValueType>> init = {}) const& {
        return makeMap(init);
    }

    template <typename ValueType>
    Map<ValueType> makeBSONEltIndexedMap(
        std::initializer_list<std::pair<const BSONElement, ValueType>> init = {}) const&& = delete;

    /**
     * Constructs an unordered map from BSONElement to type ValueType whose ordering is given by
     * this comparator. This comparator must outlive the returned map.
     */
    template <typename ValueType>
    UnorderedMap<ValueType> makeBSONEltIndexedUnorderedMap(
        std::initializer_list<std::pair<const BSONElement, ValueType>> init = {}) const& {
        return makeUnorderedMap(init);
    }

    template <typename ValueType>
    UnorderedMap<ValueType> makeBSONEltIndexedUnorderedMap(
        std::initializer_list<std::pair<const BSONElement, ValueType>> init = {}) const&& = delete;
};

using BSONEltSet = BSONComparatorInterfaceBase<BSONElement>::Set;

using BSONEltUnorderedSet = BSONComparatorInterfaceBase<BSONElement>::UnorderedSet;

template <typename ValueType>
using BSONEltIndexedMap = BSONComparatorInterfaceBase<BSONElement>::Map<ValueType>;

template <typename ValueType>
using BSONEltIndexedUnorderedMap =
    BSONComparatorInterfaceBase<BSONElement>::UnorderedMap<ValueType>;

}  // namespace mongo
