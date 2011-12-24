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

namespace mongo {

    struct AtomicUInt {
        AtomicUInt() : x(0) {}
        AtomicUInt(unsigned z) : x(z) { }

        operator unsigned() const { return x; }
        unsigned get() const { return x; }

        inline AtomicUInt operator++(); // ++prefix
        inline AtomicUInt operator++(int);// postfix++
        inline AtomicUInt operator--(); // --prefix
        inline AtomicUInt operator--(int); // postfix--

        inline void zero();

        volatile unsigned x;
    };

#if defined(_WIN32)
    void AtomicUInt::zero() { 
        InterlockedExchange((volatile long*)&x, 0);
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
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
    // this is in GCC >= 4.1
    inline void AtomicUInt::zero() { x = 0; } // TODO: this isn't thread safe - maybe
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
#elif defined(__GNUC__)  && (defined(__i386__) || defined(__x86_64__))
    inline void AtomicUInt::zero() { x = 0; } // TODO: this isn't thread safe
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
#else
#  error "unsupported compiler or platform"
#endif

} // namespace mongo
