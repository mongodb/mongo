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
 * Implementation of the AtomicIntrinsics<T>::* operations for Windows systems.
 */

#pragma once

#include <boost/utility.hpp>

namespace mongo {

    /**
     * Default instantiation of AtomicIntrinsics<>, for unsupported types.
     */
    template <typename T, typename _IsTSupported=void>
    class AtomicIntrinsics {
    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

    /**
     * Instantiation of AtomicIntrinsics<> for 32-bit word sizes (i.e., unsigned).
     */
    template <typename T>
    class AtomicIntrinsics<T, typename boost::enable_if_c<sizeof(T) == sizeof(LONG)>::type> {
    public:

        static T compareAndSwap(volatile T* dest, T expected, T newValue) {
            return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(dest),
                                              LONG(newValue),
                                              LONG(expected));
        }

        static T swap(volatile T* dest, T newValue) {
            return InterlockedExchange(reinterpret_cast<volatile LONG*>(dest), LONG(newValue));
        }

        static T load(volatile const T* value) {
            return *value;
        }

        static void store(volatile T* dest, T newValue) {
            *dest = newValue;
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(dest), LONG(increment));
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

    /**
     * Instantiation of AtomicIntrinsics<> for 64-bit word sizes.
     */
    template <typename T>
    class AtomicIntrinsics<T, typename boost::enable_if_c<sizeof(T) == sizeof(LONGLONG)>::type> {
    public:

        static T compareAndSwap(volatile T* dest, T expected, T newValue) {
            return InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(dest),
                                                LONGLONG(newValue),
                                                LONGLONG(expected));
        }

        static T swap(volatile T* dest, T newValue) {
            return InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(dest),
                                         LONGLONG(newValue));
        }

        static T load(volatile const T* value) {
            return *value;
        }

        static void store(volatile T* dest, T newValue) {
            *dest = newValue;
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return InterlockedExchangeAdd64(reinterpret_cast<volatile LONGLONG*>(dest),
                                            LONGLONG(increment));
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

}  // namespace mongo
