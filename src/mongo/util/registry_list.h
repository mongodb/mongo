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

#include <deque>
#include <memory>

#include "mongo/stdx/mutex.h"
#include "mongo/util/concepts.h"

namespace mongo {

/**
 * Simple generator-type iterator
 *
 * This Iterator caches/obscures the underlying data type to allow subclasses to wrap its more()
 * and next() functions.
 */
template <typename DataT>
class RegistryListIterator {
public:
    using DataType = DataT;
    using ElementType = typename DataT::value_type;

    explicit RegistryListIterator(DataT data) : _data{std::move(data)} {}

    virtual ~RegistryListIterator() = default;

    /**
     * Return the next element and advance the iterator
     */
    auto next() {
        if (!more()) {
            return ElementType();
        }

        return _data[_index++];
    }

    /**
     * Return true if there are more elements available
     */
    virtual bool more() const {
        return _index < _data.size();
    }

private:
    DataType _data;

    size_t _index = 0;
};

/**
 * A synchronized add-only list of elements
 *
 * A RegistryList is intended to allow concurrent iteration, insertion, and access with minimal
 * amounts of resource contention. If each element in the list is a pointer or index, the overhead
 * of "deactivated" elements is minimal.
 *
 * This class does no lifetime management for its elements besides construction and destruction. If
 * you use it to store pointers, the pointed-to memory should be immortal.
 */
TEMPLATE(typename ElementT)
REQUIRES(std::is_default_constructible_v<ElementT>)
class RegistryList {
public:
    using ElementType = ElementT;
    using DataType = std::deque<ElementT>;
    using Iterator = RegistryListIterator<DataType>;

    virtual ~RegistryList() = default;

    /**
     * Add an element to the list
     *
     * @returns  The index of the new pointer element
     */
    size_t add(ElementType ptr) {
        stdx::lock_guard lk(_m);

        _data.emplace_back(std::move(ptr));
        return _data.size() - 1;
    }

    /**
     * Returns an element at the given index within the list
     */
    ElementType at(size_t index) const {
        stdx::lock_guard lk(_m);

        if (index >= _data.size()) {
            // If index is past our synchronized end on the deque, then indexing it will be UB.
            return ElementType();
        }

        return _data[index];
    }

    /**
     * Return the total number of elements in this list
     */
    size_t size() const {
        stdx::lock_guard lk(_m);

        return _data.size();
    }

    /**
     * Return a copy of the underlying data structure
     */
    DataType data() const {
        stdx::lock_guard lk(_m);

        return _data;
    }

    /**
     * Return an iterator for this list
     *
     * This iterator copies the state of the list at the time of capture. If additional elements are
     * added after this function is invoked, they will not be visible via the Iterator.
     */
    auto iter() const {
        return Iterator(data());
    }

private:
    mutable stdx::mutex _m;  // NOLINT
    DataType _data;
};

/**
 * Wrap the basic RegistryList concept to handle weak_ptrs
 */
TEMPLATE(typename T)
REQUIRES(std::is_constructible_v<std::weak_ptr<T>>)
class WeakPtrRegistryList : public RegistryList<std::weak_ptr<T>> {
public:
    using ElementType = std::weak_ptr<T>;
    using BaseList = RegistryList<ElementType>;
    using DataType = typename BaseList::DataType;

    class Iterator final : public BaseList::Iterator {
    public:
        using BaseList::Iterator::Iterator;

        auto next() {
            return BaseList::Iterator::next().lock();
        }
    };

    virtual ~WeakPtrRegistryList() = default;

    auto add(const std::shared_ptr<T>& ptr) {
        return BaseList::add(std::weak_ptr<T>(ptr));
    }

    auto at(size_t index) const {
        return BaseList::at(index).lock();
    }

    auto iter() const {
        return Iterator(BaseList::data());
    }
};

}  // namespace mongo
