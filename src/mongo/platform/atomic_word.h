/*    Copyright 2014 10gen Inc.
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

#include <atomic>
#include <type_traits>


namespace mongo {

/**
 * Implementation of the AtomicWord interface in terms of the C++11 Atomics.
 */
template <typename _WordType>
class AtomicWord {
public:
    /**
     * Underlying value type.
     */
    typedef _WordType WordType;

    /**
     * Construct a new word with the given initial value.
     */
    explicit AtomicWord(WordType value = WordType(0)) : _value(value) {}

    /**
     * Gets the current value of this AtomicWord.
     *
     * Has acquire and release semantics.
     */
    WordType load() const {
        return _value.load();
    }

    /**
     * Gets the current value of this AtomicWord.
     *
     * Has relaxed semantics.
     */
    WordType loadRelaxed() const {
        return _value.load(std::memory_order_relaxed);
    }

    /**
     * Sets the value of this AtomicWord to "newValue".
     *
     * Has acquire and release semantics.
     */
    void store(WordType newValue) {
        return _value.store(newValue);
    }

    /**
     * Atomically swaps the current value of this with "newValue".
     *
     * Returns the old value.
     *
     * Has acquire and release semantics.
     */
    WordType swap(WordType newValue) {
        return _value.exchange(newValue);
    }

    /**
     * Atomic compare and swap.
     *
     * If this value equals "expected", sets this to "newValue".
     * Always returns the original of this.
     *
     * Has acquire and release semantics.
     */
    WordType compareAndSwap(WordType expected, WordType newValue) {
        // NOTE: Subtle: compare_exchange mutates its first argument.
        _value.compare_exchange_strong(expected, newValue);
        return expected;
    }

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     *
     * Returns the value of this before incrementing.
     *
     * Has acquire and release semantics.
     */
    WordType fetchAndAdd(WordType increment) {
        return _value.fetch_add(increment);
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     *
     * Returns the value of this before decrementing.
     *
     * Has acquire and release semantics.
     */
    WordType fetchAndSubtract(WordType decrement) {
        return _value.fetch_sub(decrement);
    }

    /**
     * Get the current value of this, add "increment" and store it, atomically.
     *
     * Returns the value of this after incrementing.
     *
     * Has acquire and release semantics.
     */
    WordType addAndFetch(WordType increment) {
        return fetchAndAdd(increment) + increment;
    }

    /**
     * Get the current value of this, subtract "decrement" and store it, atomically.
     *
     * Returns the value of this after decrementing.
     *
     * Has acquire and release semantics.
     */
    WordType subtractAndFetch(WordType decrement) {
        return fetchAndSubtract(decrement) - decrement;
    }

private:
    std::atomic<WordType> _value;  // NOLINT
};

#define _ATOMIC_WORD_DECLARE(NAME, WTYPE)                                                          \
    typedef class AtomicWord<WTYPE> NAME;                                                          \
    namespace {                                                                                    \
    static_assert(sizeof(NAME) == sizeof(WTYPE), "sizeof(NAME) == sizeof(WTYPE)");                 \
    static_assert(std::is_standard_layout<WTYPE>::value, "std::is_standard_layout<WTYPE>::value"); \
    }  // namespace

_ATOMIC_WORD_DECLARE(AtomicUInt32, unsigned);
_ATOMIC_WORD_DECLARE(AtomicUInt64, unsigned long long);
_ATOMIC_WORD_DECLARE(AtomicInt32, int);
_ATOMIC_WORD_DECLARE(AtomicInt64, long long);
_ATOMIC_WORD_DECLARE(AtomicBool, bool);
#undef _ATOMIC_WORD_DECLARE

}  // namespace mongo
