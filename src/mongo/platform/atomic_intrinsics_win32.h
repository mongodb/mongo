/*    Copyright 2012 10gen Inc.
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

/**
 * Implementation of the AtomicIntrinsics<T>::* operations for Windows systems.
 */

#pragma once

#include <boost/utility.hpp>

#include "mongo/platform/windows_basic.h"

#include <intrin.h>
#pragma intrinsic(_InterlockedCompareExchange64)

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
            MemoryBarrier();
            T result = *value;
            MemoryBarrier();
            return result;
        }

        static T loadRelaxed(volatile const T* value) {
            return *value;
        }

        static void store(volatile T* dest, T newValue) {
            MemoryBarrier();
            *dest = newValue;
            MemoryBarrier();
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(dest), LONG(increment));
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };


    namespace details {

        template <typename T, bool HaveInterlocked64Ops>
        struct InterlockedImpl64;

        // Implementation of 64-bit Interlocked operations via Windows API calls.
        template<typename T>
        struct InterlockedImpl64<T, true> {
            static T compareAndSwap(volatile T* dest, T expected, T newValue) {
                return InterlockedCompareExchange64(
                    reinterpret_cast<volatile LONGLONG*>(dest),
                    LONGLONG(newValue),
                    LONGLONG(expected));
            }

            static T swap(volatile T* dest, T newValue) {
                return InterlockedExchange64(
                    reinterpret_cast<volatile LONGLONG*>(dest),
                    LONGLONG(newValue));
            }

            static T fetchAndAdd(volatile T* dest, T increment) {
                return InterlockedExchangeAdd64(
                    reinterpret_cast<volatile LONGLONG*>(dest),
                    LONGLONG(increment));
            }
        };

        // Implementation of 64-bit Interlocked operations for systems where the API does not
        // yet provide the Interlocked...64 operations.
        template<typename T>
        struct InterlockedImpl64<T, false> {
            static T compareAndSwap(volatile T* dest, T expected, T newValue) {
                // NOTE: We must use the compiler intrinsic here: WinXP does not offer
                // InterlockedCompareExchange64 as an API call.
                return _InterlockedCompareExchange64(
                    reinterpret_cast<volatile LONGLONG*>(dest),
                    LONGLONG(newValue),
                    LONGLONG(expected));
            }

            static T swap(volatile T* dest, T newValue) {
                // NOTE: You may be tempted to replace this with
                // 'InterlockedExchange64'. Resist! It will compile just fine despite not being
                // listed in the docs as available on XP, but the compiler may replace it with
                // calls to the non-intrinsic 'InterlockedCompareExchange64', which does not
                // exist on XP. We work around this by rolling our own synthetic in terms of
                // compareAndSwap which we have explicitly formulated in terms of the compiler
                // provided _InterlockedCompareExchange64 intrinsic.
                T currentValue = *dest;
                while (true) {
                    const T result = compareAndSwap(dest, currentValue, newValue);
                    if (result == currentValue)
                        return result;
                    currentValue = result;
                }
            }

            static T fetchAndAdd(volatile T* dest, T increment) {
                // NOTE: See note for 'swap' on why we roll this ourselves.
                T currentValue = *dest;
                while (true) {
                    const T incremented = currentValue + increment;
                    const T result = compareAndSwap(dest, currentValue, incremented);
                    if (result == currentValue)
                        return result;
                    currentValue = result;
                }
            }
        };

        // On 32-bit IA-32 systems, 64-bit load and store must be implemented in terms of
        // Interlocked operations, but on 64-bit systems they support a simpler, native
        // implementation.  The LoadStoreImpl type represents the abstract implementation of
        // loading and storing 64-bit values.
        template <typename U, typename _IsTTooBig=void>
        struct LoadStoreImpl;

        // Implementation on 64-bit systems.
        template <typename U>
        struct LoadStoreImpl<U, typename boost::enable_if_c<sizeof(U) <= sizeof(void*)>::type> {
            static U load(volatile const U* value) {
                MemoryBarrier();
                U result = *value;
                MemoryBarrier();
                return result;
            }


            static void store(volatile U* dest, U newValue) {
                MemoryBarrier();
                *dest = newValue;
                MemoryBarrier();
            }
        };

        // Implementation on 32-bit systems.
        template <typename U>
        struct LoadStoreImpl<U, typename boost::disable_if_c<sizeof(U) <= sizeof(void*)>::type> {
            // NOTE: Implemented out-of-line below since the implementation relies on
            // AtomicIntrinsics.
            static U load(volatile const U* value);
            static void store(volatile U* dest, U newValue);
        };

    } // namespace details

    /**
     * Instantiation of AtomicIntrinsics<> for 64-bit word sizes.
     */
    template <typename T>
    class AtomicIntrinsics<T, typename boost::enable_if_c<sizeof(T) == sizeof(LONGLONG)>::type> {
    public:

#if defined(NTDDI_VERSION) && defined(NTDDI_WS03SP2) && (NTDDI_VERSION >= NTDDI_WS03SP2)
        static const bool kHaveInterlocked64 = true;
#else
        static const bool kHaveInterlocked64 = false;
#endif

        typedef details::InterlockedImpl64<T, kHaveInterlocked64> InterlockedImpl;
        typedef details::LoadStoreImpl<T> LoadStoreImpl;

        static T compareAndSwap(volatile T* dest, T expected, T newValue) {
            return InterlockedImpl::compareAndSwap(dest, expected, newValue);
        }

        static T swap(volatile T* dest, T newValue) {
            return InterlockedImpl::swap(dest, newValue);
        }

        static T load(volatile const T* value) {
            return LoadStoreImpl::load(value);
        }

        static void store(volatile T* dest, T newValue) {
            LoadStoreImpl::store(dest, newValue);
        }

        static T fetchAndAdd(volatile T* dest, T increment) {
            return InterlockedImpl::fetchAndAdd(dest, increment);
        }

    private:
        AtomicIntrinsics();
        ~AtomicIntrinsics();
    };

    namespace details {

        template <typename U>
        U LoadStoreImpl<U, typename boost::disable_if_c<sizeof(U) <= sizeof(void*)>::type>
        ::load(volatile const U* value) {
            return AtomicIntrinsics<U>::compareAndSwap(const_cast<volatile U*>(value),
                                                       U(0),
                                                       U(0));
        }

        template<typename U>
        void LoadStoreImpl<U, typename boost::disable_if_c<sizeof(U) <= sizeof(void*)>::type>
        ::store(volatile U* dest, U newValue) {
            AtomicIntrinsics<U>::swap(dest, newValue);
        }

    } // namespace details

}  // namespace mongo
