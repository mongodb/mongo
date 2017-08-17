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

#include "mongo/platform/basic.h"

#include <iostream>


#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

template <typename _AtomicWordType>
void testAtomicWordBasicOperations() {
    typedef typename _AtomicWordType::WordType WordType;
    _AtomicWordType w;

    ASSERT_EQUALS(WordType(0), w.load());

    w.store(1);
    ASSERT_EQUALS(WordType(1), w.load());

    ASSERT_EQUALS(WordType(1), w.swap(2));
    ASSERT_EQUALS(WordType(2), w.load());

    ASSERT_EQUALS(WordType(2), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(2), w.load());
    ASSERT_EQUALS(WordType(2), w.compareAndSwap(2, 1));
    ASSERT_EQUALS(WordType(1), w.load());

    ASSERT_EQUALS(WordType(1), w.fetchAndAdd(14));
    ASSERT_EQUALS(WordType(17), w.addAndFetch(2));
    ASSERT_EQUALS(WordType(16), w.subtractAndFetch(1));
    ASSERT_EQUALS(WordType(16), w.fetchAndSubtract(1));
    ASSERT_EQUALS(WordType(15), w.compareAndSwap(15, 0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsUnsigned32Bit) {
    typedef AtomicUInt32::WordType WordType;
    testAtomicWordBasicOperations<AtomicUInt32>();

    AtomicUInt32 w(0xdeadbeef);
    ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0xdeadbeef, 0xcafe1234));
    ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
    ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsUnsigned64Bit) {
    typedef AtomicUInt64::WordType WordType;
    testAtomicWordBasicOperations<AtomicUInt64>();

    AtomicUInt64 w(0xdeadbeefcafe1234ULL);
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234ULL), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234ULL),
                  w.compareAndSwap(0xdeadbeefcafe1234ULL, 0xfedcba9876543210ULL));
    ASSERT_EQUALS(WordType(0xfedcba9876543210ULL), w.fetchAndAdd(0xf0000000ULL));
    ASSERT_EQUALS(WordType(0xfedcba9966543210ULL), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsSigned32Bit) {
    typedef AtomicInt32::WordType WordType;
    testAtomicWordBasicOperations<AtomicInt32>();

    AtomicInt32 w(0xdeadbeef);
    ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeef), w.compareAndSwap(0xdeadbeef, 0xcafe1234));
    ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
    ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsSigned64Bit) {
    typedef AtomicInt64::WordType WordType;
    testAtomicWordBasicOperations<AtomicInt64>();

    AtomicInt64 w(0xdeadbeefcafe1234ULL);
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234LL), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234LL),
                  w.compareAndSwap(0xdeadbeefcafe1234LL, 0xfedcba9876543210LL));
    ASSERT_EQUALS(WordType(0xfedcba9876543210LL), w.fetchAndAdd(0xf0000000LL));
    ASSERT_EQUALS(WordType(0xfedcba9966543210LL), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsFloat) {
    typedef AtomicWord<float>::WordType WordType;

    AtomicWord<float> w;

    ASSERT_EQUALS(WordType(0), w.load());

    w.store(1);
    ASSERT_EQUALS(WordType(1), w.load());

    ASSERT_EQUALS(WordType(1), w.swap(2));
    ASSERT_EQUALS(WordType(2), w.load());

    ASSERT_EQUALS(WordType(2), w.compareAndSwap(0, 1));
    ASSERT_EQUALS(WordType(2), w.load());
    ASSERT_EQUALS(WordType(2), w.compareAndSwap(2, 1));
    ASSERT_EQUALS(WordType(1), w.load());

    w.store(15);
    ASSERT_EQUALS(WordType(15), w.compareAndSwap(15, 0));
    ASSERT_EQUALS(WordType(0), w.load());
}

struct Chars {
    static constexpr size_t kLength = 6;

    Chars(const char* chars = "") {
        invariant(std::strlen(chars) < kLength);
        std::strncpy(_storage.data(), chars, sizeof(_storage));
    }

    std::array<char, 6> _storage = {};

    friend bool operator==(const Chars& lhs, const Chars& rhs) {
        return lhs._storage == rhs._storage;
    }

    friend bool operator!=(const Chars& lhs, const Chars& rhs) {
        return !(lhs == rhs);
    }
};

std::ostream& operator<<(std::ostream& os, const Chars& chars) {
    return (os << chars._storage.data());
}

TEST(AtomicWordTests, BasicOperationsComplex) {
    using WordType = Chars;

    AtomicWord<WordType> checkZero(AtomicWord<WordType>::ZeroInitTag{});
    ASSERT_EQUALS(WordType(""), checkZero.load());

    AtomicWord<WordType> w;

    ASSERT_EQUALS(WordType(), w.load());

    w.store("b");
    ASSERT_EQUALS(WordType("b"), w.load());

    ASSERT_EQUALS(WordType("b"), w.swap("c"));
    ASSERT_EQUALS(WordType("c"), w.load());

    ASSERT_EQUALS(WordType("c"), w.compareAndSwap("a", "b"));
    ASSERT_EQUALS(WordType("c"), w.load());
    ASSERT_EQUALS(WordType("c"), w.compareAndSwap("c", "b"));
    ASSERT_EQUALS(WordType("b"), w.load());

    w.store("foo");
    ASSERT_EQUALS(WordType("foo"), w.compareAndSwap("foo", "bar"));
    ASSERT_EQUALS(WordType("bar"), w.load());
}

template <typename T>
void verifyAtomicityHelper() {
    ASSERT(std::atomic<T>{}.is_lock_free());                                     // NOLINT
    ASSERT(std::atomic<typename std::make_signed<T>::type>{}.is_lock_free());    // NOLINT
    ASSERT(std::atomic<typename std::make_unsigned<T>::type>{}.is_lock_free());  // NOLINT
}

template <typename... Types>
void verifyAtomicity() {
    using expander = int[];
    (void)expander{(verifyAtomicityHelper<Types>(), 0)...};
}

TEST(AtomicWordTests, StdAtomicOfIntegralIsLockFree) {
    // 2 means that they're always atomic.  Instead of 1, that means sometimes, and 0, which means
    // never.
    ASSERT_EQUALS(2, ATOMIC_CHAR_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_CHAR16_T_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_CHAR32_T_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_WCHAR_T_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_SHORT_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_INT_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_LONG_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_LLONG_LOCK_FREE);
    ASSERT_EQUALS(2, ATOMIC_POINTER_LOCK_FREE);

    verifyAtomicity<char, char16_t, char32_t, wchar_t, short, int, long, long long>();
    ASSERT(std::atomic<bool>{}.is_lock_free());  // NOLINT
}

}  // namespace
}  // namespace mongo
