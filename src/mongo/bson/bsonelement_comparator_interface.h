// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

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
class [[MONGO_MOD_OPEN]] BSONElement::ComparatorInterface
    : public BSONComparatorInterfaceBase<BSONElement> {
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
