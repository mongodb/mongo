// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <type_traits>
#include <utility>

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
class [[MONGO_MOD_PUBLIC]] LifetimeStack {
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
