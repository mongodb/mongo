/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

/**
 * This is a data structure for keeping track of sets of integers.  Specifically, it is
 * intended to handle efficient addition and membership testing of large "almost contiguous"
 * sets of integers.  An ordinary set or a bitmap gets unwieldy doing that and the performance
 * drops quickly with mere hundreds of points.
 *
 * The way it works is it's a red/black tree (like std::set) holding closed integer intervals
 * rather than points.  The key is the high end of the range (this is convenient because of
 * the way lower_bound works).  When we add a point we search for a lower_bound in the tree;
 * the range we get back will contain the point if it is already in the tree.  Otherwise we
 * add the point by extending the current range at the low end, extending the previous range
 * at the high end, coalescing the two ranges, or adding a new single-value range.
 *
 * This is similar to the way boost::interval_set works with discrete closed intervals, but
 * boost::interval_set is much more flexible and much less performant (it is worse than
 * using std::sets with individual points).
 *
 * This data structure could be made more general e.g. by creating operations to just test for
 * membership, by allowing custom allocators, allowing inserting, erasing, and/or testing for ranges
 * rather than just points, etc.  If you do that please move it to a more general part of the
 * codebase (e.g. mongo/util).
 *
 */

#pragma once

#include "mongo/util/assert_util.h"

#include <type_traits>

#include <boost/intrusive/set.hpp>

namespace mongo {
template <typename Int>
requires std::is_integral_v<Int>
class IntegerIntervalSet {
public:
    class iterator;
    using size_type = std::size_t;

    IntegerIntervalSet() = default;

    /**
     * Copy constructor.
     */
    IntegerIntervalSet(const IntegerIntervalSet& other) {
        _intervalSet.clone_from(other._intervalSet, &cloner, &disposer);
    }

    /**
     * Copy assignment operator.
     */
    IntegerIntervalSet& operator=(const IntegerIntervalSet& other) {
        _intervalSet.clone_from(other._intervalSet, &cloner, &disposer);
        return *this;
    }

    IntegerIntervalSet(IntegerIntervalSet&&) noexcept = default;
    IntegerIntervalSet& operator=(IntegerIntervalSet&&) noexcept = default;

    /**
     * Destructor. Note that destroying a boost:intrusive::set does not free its elements,
     * so the default destructor is not sufficient.
     */
    ~IntegerIntervalSet() {
        clear();
    }

    /**
     * Returns true if the interval set is empty.
     */
    bool empty() const {
        return _intervalSet.empty();
    }

    /**
     * Returns the number of intervals in the set.
     */
    size_type size() const {
        return _intervalSet.size();
    }

    /**
     * Resets the interval set to empty.
     */
    void clear() {
        _intervalSet.clear_and_dispose(&disposer);
    }

    /**
     * Returns an iterator to the first interval.
     * Note since all iterators are const, there's no separate overload for const vs non-const.
     */
    iterator begin() const {
        return iterator(_intervalSet.begin());
    }

    /**
     * Returns a past-the-end iterator.
     */
    iterator end() const {
        return iterator(_intervalSet.end());
    }

    /**
     * Insert a single integer into the set.
     *
     * Like the STL containers, returns an iterator indicating where the element is, and
     * a boolean indicating whether it was inserted (true) or already there (false).
     */
    std::pair<iterator, bool> insert(Int value) {
        auto loc = _intervalSet.lower_bound(value);
        // loc points to smallest element with 'interval.second' >= value. If the value is in the
        // set, it will be in this interval.
        if (loc != _intervalSet.end() && value >= loc->interval.first) {
            // The value is in this interval.
            return {iterator(loc), false};
        }
        // loc == end or (loc->second >= loc->first > value).
        // loc == begin or prev(loc)->first <= prev(loc)->second < value (by definition of
        // lower bound).
        // So there are four alternatives if loc and prev(loc) both exist.
        // value == loc->first - 1 == prev(loc)->second + 1 <-- coalesce prev(loc) with loc.
        // value == loc->first - 1 > prev(loc)->second + 1 <- extend loc
        // value == prev(loc)->second + 1 < loc->first - 1 <- extend prev(loc)
        // value > prev(loc)->second + 1 && value < loc->first - 1 <-- new item.
        auto prev = (loc == _intervalSet.begin()) ? _intervalSet.end() : std::prev(loc);
        // Set bit 0 of selector to 1 if value should be part of 'loc'.
        unsigned selector = (loc != _intervalSet.end() && value == loc->interval.first - 1) ? 1 : 0;
        // Set bit 1 of selector to 1 if value should be part of 'prev'
        selector |= (prev != _intervalSet.end() && value == prev->interval.second + 1) ? 2 : 0;
        // At the end of this switch, 'loc' must be an iterator pointing to the range containing
        // 'value'.
        switch (selector) {
            case 0:
                // value does not merge with an existing interval.
                loc = _intervalSet.insert_before(loc, *(newNode(value, value)));
                break;
            case 1:
                // value merges with 'loc', but not 'prev'
                loc->interval.first = value;
                break;
            case 2:
                // value merges with 'prev', but not 'loc'
                // Note this changes the node's key.  But it is safe because it does not change
                // the relative ordering of the key with respect to other keys.  Allowing
                // in-place changing of keys is why boost::intrusive is used.
                prev->interval.second = value;
                loc = prev;
                break;
            case 3:
                // prev and loc merge.
                loc->interval.first = prev->interval.first;
                _intervalSet.erase_and_dispose(prev, &disposer);
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return {iterator(loc), true};
    }

    /**
     * Remove the value from the interval set, splitting as necessary.
     * Returns the number of values removed (always 0 or 1), to match the STL erase interface.
     */
    size_type erase(Int value) {
        auto loc = _intervalSet.lower_bound(value);
        // loc points to smallest element with 'interval.second' >= value. If the value is in the
        // set, it will be in this interval.
        if (loc == _intervalSet.end() || value < loc->interval.first) {
            // The value is not in the set.
            return 0;
        }
        // Bit 0 of selector indicates whether we're erasing from the start of the range.
        unsigned selector = (value == loc->interval.first) ? 1 : 0;
        // Bit 1 of selector indicates whether we're erasing from the end of the range.
        selector |= (value == loc->interval.second) ? 2 : 0;
        switch (selector) {
            case 0:
                // Splitting the range. Add the new range, then shorten the existing range.
                // (It is valid to do it in this order because boost::intrusive iterators
                // aren't invalidated by inserts)
                _intervalSet.insert_before(loc, *(newNode(loc->interval.first, value - 1)));
                loc->interval.first = value + 1;
                break;
            case 1:
                // Erasing from the start of the range.
                loc->interval.first = value + 1;
                break;
            case 2:
                // Erasing from the end of the range.
                loc->interval.second = value - 1;
                break;
            case 3:
                // Erasing a single-element range
                _intervalSet.erase_and_dispose(loc, &disposer);
                break;
            default:
                MONGO_UNREACHABLE;
        }
        // The value was in the set and has been erased.
        return 1;
    }

private:
    // The set_base_hook<> is boost::intrusive::set boilerplate which provides the red-black tree
    // color and node pointers, and some methods to manipulate them (used internally by boost).
    struct NodeType : public boost::intrusive::set_base_hook<> {
        NodeType() = default;
        NodeType(const NodeType&) = default;
        NodeType& operator=(const NodeType&) = default;
        NodeType(Int a, Int b) : interval{a, b} {}

        std::pair<Int, Int> interval;  // {first, second} corresponds to [low, high]
    };

    // This class is used to tell boost::intrusive::set to treat the second element of the interval
    // as the key to the set's map-like interface.
    struct HighIsKey {
        // This typedef is boilerplate for boost::intrusive::set which tells boost the type of the
        // key.
        typedef Int type;

        type operator()(const NodeType& node) const {
            return node.interval.second;
        }
    };

    // This typedef defines a boost::intrusive::set with nodes of the type NodeType and providing a
    // map-like interface with a key defined by the type HighIsKey.
    typedef boost::intrusive::set<NodeType, boost::intrusive::key_of_value<HighIsKey>>
        IntervalSetType;

    // Creates a new node containing the range given.  This is here to keep the memory
    // allocation and freeing all in one place.
    NodeType* newNode(Int low, Int high) {
        return new NodeType(low, high);
    }

    // Makes a copy of NodeType objects; used to implement the copy constructor/operator
    // using boost::intrusive clone_from.
    static NodeType* cloner(const NodeType& node) {
        return new NodeType(node);
    }

    // Disposes of NodeType objects, used as the Disposer argument to erase_and_dispose,
    // clear_and_dispose, and clone_from.
    static void disposer(NodeType* nodePtr) {
        delete nodePtr;
    }

    IntervalSetType _intervalSet;

public:
    /**
     * The STL-style iterator for this map.
     * Users may not change intervals through the iterator, since doing so could violate
     * set ordering invariants, the invariant that (first <= last), and the invariant that
     * the intervals are in a "minimal representation" -- no intervals overlap, even at a point.
     */
    class iterator {
    public:
        using value_type = std::pair<Int, Int>;
        using difference_type = std::ptrdiff_t;
        iterator() = default;

        const std::pair<Int, Int>& operator*() const {
            return _iter->interval;
        }
        const std::pair<Int, Int>* operator->() const {
            return &_iter->interval;
        }

        // This is the prefix operator++ (i.e. "++iter")
        iterator& operator++() {
            ++_iter;
            return *this;
        }

        // This is the postfix operator++ (i.e. "iter++")
        iterator operator++(int) {
            auto oldIter = _iter;
            ++(*this);
            return iterator(oldIter);
        }

        // This is the prefix operator-- (i.e. "--iter")
        iterator& operator--() {
            --_iter;
            return *this;
        }

        // This is the postfix operator-- (i.e. "--iter")
        iterator operator--(int) {
            auto oldIter = _iter;
            --(*this);
            return iterator(oldIter);
        }

        bool operator==(const iterator& other) const {
            return _iter == other._iter;
        }

    private:
        friend class IntegerIntervalSet;
        explicit iterator(const typename IntervalSetType::const_iterator& iter) : _iter(iter) {}
        typename IntervalSetType::const_iterator _iter;
    };
};

}  // namespace mongo
