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

/**
 * Implementation of the AtomicIntrinsics<T>::* operations for systems on any
 * architecture using a GCC 4.1+ compatible compiler toolchain.
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
            return __sync_val_compare_and_swap(dest, expected, newValue);
        }

        static T swap(volatile T* dest, T newValue) {
            T currentValue = *dest;
            while (true) {
                const T result = compareAndSwap(dest, currentValue, newValue);
                if (result == currentValue)
                    return result;
                currentValue = result;
            }
        }

        static T load(volatile const T* value) {
            __sync_synchronize();
            T result = *value;
            __sync_synchronize();
            return result;
        }

        static T loadRelaxed(volatile const T* value) {
            asm volatile("" ::: "memory");
            return *value;
        }

        static void store(volatile T* dest, T newValue) {
            __sync_synchronize();
            *dest = newValue;
            __sync_synchronize();
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return __sync_fetch_and_add(dest, increment);
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

    template <typename T>
    class AtomicIntrinsics<T, typename boost::disable_if_c<sizeof(T) <= sizeof(void*)>::type> {
    public:

        static T compareAndSwap(volatile T* dest, T expected, T newValue) {
            return __sync_val_compare_and_swap(dest, expected, newValue);
        }

        static T swap(volatile T* dest, T newValue) {
            T currentValue = *dest;
            while (true) {
                const T result = compareAndSwap(dest, currentValue, newValue);
                if (result == currentValue)
                    return result;
                currentValue = result;
            }
        }

        static T load(volatile const T* value) {
            return compareAndSwap(const_cast<volatile T*>(value), T(0), T(0));
        }

        static void store(volatile T* dest, T newValue) {
            swap(dest, newValue);
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return __sync_fetch_and_add(dest, increment);
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

}  // namespace mongo
