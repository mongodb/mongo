/*    Copyright 2014 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#if !defined(MONGO_HAVE_CXX11_ATOMICS)
#error "Cannot use atomic_word_cxx11.h without C++11 <atomic> support"
#endif

#include <atomic>

#include <boost/static_assert.hpp>

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
        explicit AtomicWord(WordType value=WordType(0)) : _value(value) {}

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
        std::atomic<WordType> _value;
    };

#define _ATOMIC_WORD_DECLARE(NAME, WTYPE)                               \
    typedef class AtomicWord<WTYPE> NAME;                               \
    namespace { BOOST_STATIC_ASSERT(sizeof(NAME) == sizeof(WTYPE)); }

    _ATOMIC_WORD_DECLARE(AtomicUInt32, unsigned);
    _ATOMIC_WORD_DECLARE(AtomicUInt64, unsigned long long);
    _ATOMIC_WORD_DECLARE(AtomicInt32, int);
    _ATOMIC_WORD_DECLARE(AtomicInt64, long long);
#undef _ATOMIC_WORD_DECLARE

} // namespace mongo

