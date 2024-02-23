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

#include <atomic>
#include <cstring>
#include <type_traits>

#include "mongo/base/static_assert.h"
#include "mongo/stdx/type_traits.h"

namespace mongo {

namespace atomic_word_detail {

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
 * Implementation of the AtomicWord interface in terms of the C++11 Atomics.
 * Defines the operations provided by a non-incrementable AtomicWord.
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
     * Construct a new word, default-initialized.
     */
    constexpr Base() : _value() {}

    /**
     * Construct a new word with the given initial value.
     */
    explicit(false) constexpr Base(WordType v) : _value(v) {}

    /**
     * Gets the current value of this AtomicWord.
     */
    WordType load() const {
        return _value.load();
    }

    /**
     * Gets the current value of this AtomicWord using relaxed memory order.
     */
    WordType loadRelaxed() const {
        return _value.load(std::memory_order_relaxed);
    }

    /**
     * Sets the value of this AtomicWord to "newValue".
     */
    void store(WordType newValue) {
        _value.store(newValue);
    }

    /**
     * Sets the value of this AtomicWord to "newValue" using relaxed memory order.
     */
    void storeRelaxed(WordType newValue) {
        _value.store(newValue, std::memory_order_relaxed);
    }

    /**
     * Atomically swaps the current value of this with "newValue".
     *
     * Returns the old value.
     */
    WordType swap(WordType newValue) {
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
    bool compareAndSwap(WordType* expected, WordType newValue) {
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
    using WordType = typename Parent::WordType;
    using Parent::Parent;

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     *
     * Returns the value of this before incrementing.
     */
    WordType fetchAndAdd(WordType increment) {
        return _value.fetch_add(increment);
    }

    /**
     * Like "fetchAndAdd", but with relaxed memory order. Appropriate where relative
     * order of operations doesn't matter. A stat counter, for example.
     */
    WordType fetchAndAddRelaxed(WordType increment) {
        return _value.fetch_add(increment, std::memory_order_relaxed);
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this before decrementing.
     */
    WordType fetchAndSubtract(WordType decrement) {
        return _value.fetch_sub(decrement);
    }

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     * Returns the value of this after incrementing.
     */
    WordType addAndFetch(WordType increment) {
        return fetchAndAdd(increment) + increment;
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this after decrementing.
     */
    WordType subtractAndFetch(WordType decrement) {
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
    WordType fetchAndBitAnd(WordType bits) {
        return _value.fetch_and(bits);
    }

    /**
     * Atomically compute and store 'load() | bits'
     *
     * Returns the value of this before bitor-ing.
     */
    WordType fetchAndBitOr(WordType bits) {
        return _value.fetch_or(bits);
    }

    /**
     * Atomically compute and store 'load() ^ bits'
     *
     * Returns the value of this before bitxor-ing.
     */
    WordType fetchAndBitXor(WordType bits) {
        return _value.fetch_xor(bits);
    }
};

}  // namespace atomic_word_detail

/**
 * Instantiations of AtomicWord must be trivially copyable.
 */
template <typename T>
class AtomicWord : public atomic_word_detail::Base<T> {
public:
    using atomic_word_detail::Base<T>::Base;
};

}  // namespace mongo
