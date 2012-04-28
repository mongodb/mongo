// atomic_int.h
// atomic wrapper for unsigned

/*    Copyright 2009 10gen Inc.
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

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(__APPLE__)
#  include <libkern/OSAtomic.h>
#endif

#include "mongo/platform/compiler.h"

namespace mongo {

    /**
     * An unsigned integer supporting atomic read-modify-write operations.
     *
     * Many operations on these types depend on natural alignment (4 byte alignment for 4-byte
     * words, i.e.).
     */
    struct MONGO_COMPILER_ALIGN_TYPE( 4 ) AtomicUInt {
        AtomicUInt() : x(0) {}
        AtomicUInt(unsigned z) : x(z) { }

        operator unsigned() const { return x; }
        unsigned get() const { return x; }
        inline void set(unsigned newX);

        inline AtomicUInt operator++(); // ++prefix
        inline AtomicUInt operator++(int);// postfix++
        inline AtomicUInt operator--(); // --prefix
        inline AtomicUInt operator--(int); // postfix--
        inline void signedAdd(int by);
        inline void zero() { set(0); }
        volatile unsigned x;
    };

#if defined(_WIN32)
    void AtomicUInt::set(unsigned newX) {
        InterlockedExchange((volatile long *)&x, newX);
    }

    AtomicUInt AtomicUInt::operator++() {
        return InterlockedIncrement((volatile long*)&x);
    }
    AtomicUInt AtomicUInt::operator++(int) {
        return InterlockedIncrement((volatile long*)&x)-1;
    }
    AtomicUInt AtomicUInt::operator--() {
        return InterlockedDecrement((volatile long*)&x);
    }
    AtomicUInt AtomicUInt::operator--(int) {
        return InterlockedDecrement((volatile long*)&x)+1;
    }
# if defined(_WIN64)
    // don't see an InterlockedAdd for _WIN32...hmmm
    void AtomicUInt::signedAdd(int by) {
        InterlockedAdd((volatile long *)&x,by);
    }
# endif
#elif defined(HAVE_SYNC_FETCH_AND_ADD)
    // this is in GCC >= 4.1
    inline void AtomicUInt::set(unsigned newX) { __sync_synchronize(); x = newX; }
    AtomicUInt AtomicUInt::operator++() {
        return __sync_add_and_fetch(&x, 1);
    }
    AtomicUInt AtomicUInt::operator++(int) {
        return __sync_fetch_and_add(&x, 1);
    }
    AtomicUInt AtomicUInt::operator--() {
        return __sync_add_and_fetch(&x, -1);
    }
    AtomicUInt AtomicUInt::operator--(int) {
        return __sync_fetch_and_add(&x, -1);
    }
    void AtomicUInt::signedAdd(int by) {
        __sync_fetch_and_add(&x, by);
    }
#elif defined(__GNUC__)  && (defined(__i386__) || defined(__x86_64__))
    inline void AtomicUInt::set(unsigned newX) {
        asm volatile("mfence" ::: "memory");
        x = newX;
    }

    // from boost 1.39 interprocess/detail/atomic.hpp
    inline unsigned atomic_int_helper(volatile unsigned *x, int val) {
        int r;
        asm volatile
        (
            "lock\n\t"
            "xadd %1, %0":
            "+m"( *x ), "=r"( r ): // outputs (%0, %1)
            "1"( val ): // inputs (%2 == %1)
            "memory", "cc" // clobbers
        );
        return r;
    }
    AtomicUInt AtomicUInt::operator++() {
        return atomic_int_helper(&x, 1)+1;
    }
    AtomicUInt AtomicUInt::operator++(int) {
        return atomic_int_helper(&x, 1);
    }
    AtomicUInt AtomicUInt::operator--() {
        return atomic_int_helper(&x, -1)-1;
    }
    AtomicUInt AtomicUInt::operator--(int) {
        return atomic_int_helper(&x, -1);
    }
    void AtomicUInt::signedAdd(int by) {
        atomic_int_helper(&x, by);
    }
#elif defined(__APPLE__)
#define PTR_CAST reinterpret_cast<volatile int32_t*>( &x )

    AtomicUInt AtomicUInt::operator++() {
        // OSAtomicIncrement32Barrier  returns the new value
        // TODO: Is the barrier version needed?
        return OSAtomicIncrement32Barrier( PTR_CAST );
    }
    AtomicUInt AtomicUInt::operator++(int) {
        return OSAtomicIncrement32Barrier( PTR_CAST ) - 1;
    }
    AtomicUInt AtomicUInt::operator--() {
        return OSAtomicDecrement32Barrier( PTR_CAST );
    }
    AtomicUInt AtomicUInt::operator--(int) {
        return OSAtomicDecrement32Barrier( PTR_CAST ) + 1;
    }

    void AtomicUInt::signedAdd( int by ) {
        OSAtomicAdd32Barrier( by, PTR_CAST );
    }

    void AtomicUInt::set( unsigned newX ) {
        x = newX;
        OSMemoryBarrier();
    }

#undef PTR_CAST

#else
#  error "unsupported compiler or platform"
#endif

} // namespace mongo
