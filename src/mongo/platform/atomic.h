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

#include "mongo/base/static_assert.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <cstring>
#include <type_traits>

namespace MONGO_MOD_PUB mongo {

namespace atomic_detail {

enum class Category { kBasic, kArithmetic, kUnsigned };

template <typename T>
constexpr Category getCategory() {
    if (std::is_integral<T>() && !std::is_same<T, bool>()) {
        if (std::is_unsigned<T>() && !std::is_same<T, char>()) {
            return Category::kUnsigned;
        }
        return Category::kArithmetic;
    }
    return Category::kBasic;
}

template <typename T, Category = getCategory<T>()>
class Base;

/**
 * Implementation of the Atomic interface in terms of the C++11 Atomics.
 * Defines the operations provided by a non-incrementable Atomic.
 * All operations have sequentially consistent semantics unless otherwise noted.
 */
template <typename T>
class Base<T, Category::kBasic> {
public:
    /**
     * Underlying value type.
     */
    using WordType = T;

    /**
     * Construct a new word, value-initialized.
     */
    constexpr Base() : _value() {}

    /**
     * Construct a new word with the given initial value.
     */
    explicit(false) constexpr Base(WordType v) : _value(v) {}

    /**
     * Gets the current value of this Atomic.
     */
    MONGO_MOD_PUB WordType load() const {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ
        return _value.load();
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

    /**
     * Gets the current value of this Atomic using relaxed memory order.
     */
    MONGO_MOD_PUB WordType loadRelaxed() const {
        return _value.load(std::memory_order_relaxed);
    }

    /**
     * Sets the value of this Atomic to "newValue".
     */
    MONGO_MOD_PUB void store(WordType newValue) {
        _value.store(newValue);
    }

    /**
     * Sets the value of this Atomic to "newValue" using relaxed memory order.
     */
    MONGO_MOD_PUB void storeRelaxed(WordType newValue) {
        _value.store(newValue, std::memory_order_relaxed);
    }

    /**
     * Atomically swaps the current value of this with "newValue".
     *
     * Returns the old value.
     */
    MONGO_MOD_PUB WordType swap(WordType newValue) {
        return _value.exchange(newValue);
    }

    /**
     * Atomic compare and swap.
     *
     * If this value equals the value at "expected", sets this value to "newValue".
     * Otherwise, sets the storage at "expected" to this value.
     *
     * Returns true if swap successful, false otherwise
     */
    MONGO_MOD_PUB bool compareAndSwap(WordType* expected, WordType newValue) {
        // NOTE: Subtle: compare_exchange mutates its first argument.
        return _value.compare_exchange_strong(*expected, newValue);
    }

protected:
    // At least with GCC 10, this assertion fails for small types like bool.
#if !defined(__riscv)
    MONGO_STATIC_ASSERT(std::atomic<WordType>::is_always_lock_free);  // NOLINT
#endif

    std::atomic<WordType> _value;  // NOLINT
};

/**
 * Has the basic operations, plus some arithmetic operations.
 */
template <typename T>
class Base<T, Category::kArithmetic> : public Base<T, Category::kBasic> {
protected:
    using Parent = Base<T, Category::kBasic>;
    using Parent::_value;

public:
    using WordType MONGO_MOD_PUB = typename Parent::WordType;
    using Parent::Parent;

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     *
     * Returns the value of this before incrementing.
     */
    MONGO_MOD_PUB WordType fetchAndAdd(WordType increment) {
        return _value.fetch_add(increment);
    }

    /**
     * Like "fetchAndAdd", but with relaxed memory order. Appropriate where relative
     * order of operations doesn't matter. A stat counter, for example.
     */
    MONGO_MOD_PUB WordType fetchAndAddRelaxed(WordType increment) {
        return _value.fetch_add(increment, std::memory_order_relaxed);
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this before decrementing.
     */
    MONGO_MOD_PUB WordType fetchAndSubtract(WordType decrement) {
        return _value.fetch_sub(decrement);
    }

    /**
     * Like "fetchAndSubtract", but with relaxed memory order. Appropriate where relative
     * order of operations doesn't matter. A stat counter, for example.
     */
    MONGO_MOD_PUB WordType fetchAndSubtractRelaxed(WordType decrement) {
        return _value.fetch_sub(decrement, std::memory_order_relaxed);
    }

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     * Returns the value of this after incrementing.
     */
    MONGO_MOD_PUB WordType addAndFetch(WordType increment) {
        return fetchAndAdd(increment) + increment;
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this after decrementing.
     */
    MONGO_MOD_PUB WordType subtractAndFetch(WordType decrement) {
        return fetchAndSubtract(decrement) - decrement;
    }
};

template <typename T>
class Base<T, Category::kUnsigned> : public Base<T, Category::kArithmetic> {
private:
    using Parent = Base<T, Category::kArithmetic>;
    using Parent::_value;

public:
    using WordType = typename Parent::WordType;
    using Parent::Parent;

    /**
     * Atomically compute and store 'load() & bits'
     *
     * Returns the value of this before bitand-ing.
     */
    MONGO_MOD_PUB WordType fetchAndBitAnd(WordType bits) {
        return _value.fetch_and(bits);
    }

    /**
     * Atomically compute and store 'load() | bits'
     *
     * Returns the value of this before bitor-ing.
     */
    MONGO_MOD_PUB WordType fetchAndBitOr(WordType bits) {
        return _value.fetch_or(bits);
    }

    /**
     * Atomically compute and store 'load() ^ bits'
     *
     * Returns the value of this before bitxor-ing.
     */
    MONGO_MOD_PUB WordType fetchAndBitXor(WordType bits) {
        return _value.fetch_xor(bits);
    }
};

}  // namespace atomic_detail

/**
 * Our wrapper around std::atomic.
 *
 * While this class originally predated std::atomic, we still prefer using it because it makes our
 * code easier to review. In particular, it doesn't try to have the same API as the underlying T and
 * instead makes all accesses explicit. This makes it easer to read and review code because all
 * synchronization points and potentially concurrent accesses are explicitly marked.
 */
template <typename T>
class Atomic : public atomic_detail::Base<T> {
public:
    using atomic_detail::Base<T>::Base;
};

}  // namespace MONGO_MOD_PUB mongo
