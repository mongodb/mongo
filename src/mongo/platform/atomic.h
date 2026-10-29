// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/static_assert.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <cstring>
#include <type_traits>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace atomic_detail {

enum class Category { kBasic, kArithmetic, kUnsigned };

template <typename T>
constexpr Category getCategory() {
    if (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        if (std::is_unsigned_v<T> && !std::is_same_v<T, char>) {
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
    [[MONGO_MOD_PUBLIC]] WordType load() const {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_WORKAROUND_ATOMIC_READ
        return _value.load();
        MONGO_COMPILER_DIAGNOSTIC_POP
    }

    /**
     * Gets the current value of this Atomic using relaxed memory order.
     */
    [[MONGO_MOD_PUBLIC]] WordType loadRelaxed() const {
        return _value.load(std::memory_order_relaxed);
    }

    /**
     * Sets the value of this Atomic to "newValue".
     */
    [[MONGO_MOD_PUBLIC]] void store(WordType newValue) {
        _value.store(newValue);
    }

    /**
     * Sets the value of this Atomic to "newValue" using relaxed memory order.
     */
    [[MONGO_MOD_PUBLIC]] void storeRelaxed(WordType newValue) {
        _value.store(newValue, std::memory_order_relaxed);
    }

    /**
     * Atomically swaps the current value of this with "newValue".
     *
     * Returns the old value.
     */
    [[MONGO_MOD_PUBLIC]] WordType swap(WordType newValue) {
        return _value.exchange(newValue);
    }

    /**
     * Atomic compare and swap (strong).
     *
     * If this value equals the value at "expected", sets this value to "newValue".
     * Otherwise, sets the storage at "expected" to this value.
     *
     * Returns true if swap successful, false otherwise
     */
    [[MONGO_MOD_PUBLIC]] bool compareAndSwap(WordType* expected, WordType newValue) {
        // NOTE: Subtle: compare_exchange mutates its first argument.
        return _value.compare_exchange_strong(*expected, newValue);
    }

    /**
     * Atomic compare and swap (weak).
     *
     * If this value equals the value at "expected", sets this value to "newValue".
     * Otherwise, sets the storage at "expected" to this value.
     *
     * compare_exchange_weak is allowed to fail spuriously, that is, acts as if *this != expected
     * even if they are equal. Prefer the above "strong" version unless there is a compelling reason
     * to use this one (stats-gathering, for example, is a reasonable time to use this).
     *
     * Returns true if swap successful, false otherwise
     */
    [[MONGO_MOD_PUBLIC]] bool compareAndSwapWeak(WordType* expected, WordType newValue) {
        // NOTE: Subtle: compare_exchange mutates its first argument.
        return _value.compare_exchange_weak(*expected, newValue);
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
    using WordType [[MONGO_MOD_PUBLIC]] = typename Parent::WordType;
    using Parent::Parent;

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     *
     * Returns the value of this before incrementing.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndAdd(WordType increment) {
        return _value.fetch_add(increment);
    }

    /**
     * Like "fetchAndAdd", but with relaxed memory order. Appropriate where relative
     * order of operations doesn't matter. A stat counter, for example.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndAddRelaxed(WordType increment) {
        return _value.fetch_add(increment, std::memory_order_relaxed);
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this before decrementing.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndSubtract(WordType decrement) {
        return _value.fetch_sub(decrement);
    }

    /**
     * Like "fetchAndSubtract", but with relaxed memory order. Appropriate where relative
     * order of operations doesn't matter. A stat counter, for example.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndSubtractRelaxed(WordType decrement) {
        return _value.fetch_sub(decrement, std::memory_order_relaxed);
    }

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     * Returns the value of this after incrementing.
     */
    [[MONGO_MOD_PUBLIC]] WordType addAndFetch(WordType increment) {
        return fetchAndAdd(increment) + increment;
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     * Returns the value of this after decrementing.
     */
    [[MONGO_MOD_PUBLIC]] WordType subtractAndFetch(WordType decrement) {
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
    [[MONGO_MOD_PUBLIC]] WordType fetchAndBitAnd(WordType bits) {
        return _value.fetch_and(bits);
    }

    /**
     * Atomically compute and store 'load() | bits'
     *
     * Returns the value of this before bitor-ing.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndBitOr(WordType bits) {
        return _value.fetch_or(bits);
    }

    /**
     * Atomically compute and store 'load() ^ bits'
     *
     * Returns the value of this before bitxor-ing.
     */
    [[MONGO_MOD_PUBLIC]] WordType fetchAndBitXor(WordType bits) {
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

/// Temporary shim from deprecated name for Atomic.
template <typename T>
using AtomicWord = Atomic<T>;
}  // namespace mongo
