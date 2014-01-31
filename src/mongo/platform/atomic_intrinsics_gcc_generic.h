/*    Copyright 2012 10gen Inc.
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

/**
 * Implementation of the AtomicIntrinsics<T>::* operations for systems on any
 * architecture using a new enough GCC-compatible compiler toolchain.
 */

#pragma once

#include <boost/utility.hpp>

namespace mongo {

    /**
     * Instantiation of AtomicIntrinsics<> for all word types T.
     */
    template <typename T, typename IsTLarge=void>
    class AtomicIntrinsics {
    public:

        static T compareAndSwap(volatile T* dest, T expected, T newValue) {
            __atomic_compare_exchange(dest, &expected, &newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return expected;
        }

        static T swap(volatile T* dest, T newValue) {
            T result;
            __atomic_exchange(dest, &newValue, &result, __ATOMIC_SEQ_CST);
            return result;
        }

        static T load(volatile const T* value) {
            T result;
            __atomic_load(value, &result, __ATOMIC_SEQ_CST);
            return result;
        }

        static T loadRelaxed(volatile const T* value) {
            T result;
            __atomic_load(value, &result, __ATOMIC_RELAXED);
            return result;
        }

        static void store(volatile T* dest, T newValue) {
            __atomic_store(dest, &newValue, __ATOMIC_SEQ_CST);
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return __atomic_fetch_add(dest, increment, __ATOMIC_SEQ_CST);
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

}  // namespace mongo
