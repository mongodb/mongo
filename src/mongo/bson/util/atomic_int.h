// atomic_int.h
// atomic wrapper for unsigned

/*    Copyright 2009 10gen Inc.
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

#if defined(_WIN32)
#include "mongo/platform/windows_basic.h"
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
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
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
#else
#  error "unsupported compiler or platform"
#endif

} // namespace mongo
