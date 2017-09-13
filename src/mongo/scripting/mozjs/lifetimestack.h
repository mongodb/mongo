/*    Copyright 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace mozjs {

/**
 * Implements a stack with:
 *   o specific LIFO lifetime guarantees
 *   o builtin storage (based on template parameter)
 *
 * This is useful for manipulating stacks of types which are non-movable and
 * non-copyable (and thus cannot be put into standard containers). The lack of
 * an allocator additionally supports types that must live in particular
 * region of memory (like the stack vs. the heap).
 *
 * We need this to store GC Rooting types from spidermonkey safely.
 */
template <typename T, std::size_t N>
class LifetimeStack {
public:
    // Boiler plate typedefs
    using value_type = T;
    using size_type = std::size_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = T*;
    using const_pointer = const T*;

    LifetimeStack() = default;

    ~LifetimeStack() {
        while (size()) {
            pop();
        }
    }

    /**
     * Stacks are non-copyable and non-movable
     */
    LifetimeStack(const LifetimeStack&) = delete;
    LifetimeStack& operator=(const LifetimeStack&) = delete;

    LifetimeStack(LifetimeStack&& other) = delete;
    LifetimeStack& operator=(LifetimeStack&& other) = delete;

    template <typename... Args>
    void emplace(Args&&... args) {
        invariant(_size <= N);

        new (_data() + _size) T(std::forward<Args>(args)...);

        _size++;
    }

    void pop() {
        invariant(_size > 0);

        (&top())->~T();

        _size--;
    }

    const_reference top() const {
        invariant(_size > 0);
        return _data()[_size - 1];
    }

    reference top() {
        invariant(_size > 0);
        return _data()[_size - 1];
    }

    size_type size() const {
        return _size;
    }

    bool empty() const {
        return _size == 0;
    }

    size_type capacity() const {
        return N;
    }

private:
    pointer _data() {
        return reinterpret_cast<pointer>(&_storage);
    }

    const_pointer _data() const {
        return reinterpret_cast<const_pointer>(&_storage);
    }

private:
    typename std::aligned_storage<sizeof(T) * N, std::alignment_of<T>::value>::type _storage;

    size_type _size = 0;
};

}  // namespace mozjs
}  // namespace mongo
