/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <fmt/format.h>
#include <iostream>
#include <memory>


#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {
namespace {

/*
 * Modified CAS for AtomicWord to return boolean based on whether or not the swap occurred.
 * This helper function mimics the old implementation, which returned the original value of
 * expected.
 */
template <typename WordType>
WordType testAtomicWordCompareAndSwap(AtomicWord<WordType>& word,
                                      WordType expected,
                                      WordType desired) {
    auto prevWord = word.loadRelaxed();
    auto didSwap = word.compareAndSwap(&expected, desired);
    ASSERT_EQUALS(expected, prevWord);
    if (didSwap) {
        ASSERT_EQUALS(word.load(), desired);
    } else {
        ASSERT_EQUALS(word.load(), prevWord);
    }
    return expected;
}

template <typename _AtomicWordType>
void testAtomicWordBasicOperations() {
    typedef typename _AtomicWordType::WordType WordType;
    _AtomicWordType w;

    ASSERT_EQUALS(WordType(0), w.load());

    w.store(1);
    ASSERT_EQUALS(WordType(1), w.load());

    ASSERT_EQUALS(WordType(1), w.swap(2));
    ASSERT_EQUALS(WordType(2), w.load());

    ASSERT_EQUALS(WordType(2), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(WordType(2), w.load());
    ASSERT_EQUALS(WordType(2), testAtomicWordCompareAndSwap<WordType>(w, 2, 1));
    ASSERT_EQUALS(WordType(1), w.load());

    ASSERT_EQUALS(WordType(1), w.fetchAndAdd(14));
    ASSERT_EQUALS(WordType(17), w.addAndFetch(2));
    ASSERT_EQUALS(WordType(16), w.subtractAndFetch(1));
    ASSERT_EQUALS(WordType(16), w.fetchAndSubtract(1));
    ASSERT_EQUALS(WordType(15), testAtomicWordCompareAndSwap<WordType>(w, 15, 0));
    ASSERT_EQUALS(WordType(0), w.load());
}

template <typename AtomicWordType>
void testAtomicWordBitOperations() {
    typedef typename AtomicWordType::WordType WordType;
    AtomicWordType w;

    WordType highBit = 1ull << ((sizeof(WordType) * 8) - 1);

    w.store(highBit | 0xFFull);
    ASSERT_EQUALS(WordType(highBit | 0xFFull), w.fetchAndBitAnd(highBit));
    ASSERT_EQUALS(WordType(highBit), w.fetchAndBitOr(highBit | 0xFFull));
    ASSERT_EQUALS(WordType(highBit | 0xFFull), w.fetchAndBitXor(0xFFFF));
    ASSERT_EQUALS(WordType(highBit | 0xFF00ull), w.load());
}

ASSERT_DOES_NOT_COMPILE(CharFetchAndBitAnd, typename T = int, AtomicWord<T>().fetchAndBitAnd(0));
ASSERT_DOES_NOT_COMPILE(CharFetchAndBitOr, typename T = int, AtomicWord<T>().fetchAndBitOr(0));
ASSERT_DOES_NOT_COMPILE(CharFetchAndBitXor, typename T = int, AtomicWord<T>().fetchAndBitXor(0));

ASSERT_DOES_NOT_COMPILE(IntFetchAndBitAnd, typename T = char, AtomicWord<T>().fetchAndBitAnd(0));
ASSERT_DOES_NOT_COMPILE(IntFetchAndBitOr, typename T = char, AtomicWord<T>().fetchAndBitOr(0));
ASSERT_DOES_NOT_COMPILE(IntFetchAndBitXor, typename T = char, AtomicWord<T>().fetchAndBitXor(0));

enum TestEnum { E0, E1, E2, E3 };

TEST(AtomicWordTests, BasicOperationsEnum) {
    MONGO_STATIC_ASSERT(sizeof(AtomicWord<TestEnum>) == sizeof(TestEnum));
    AtomicWord<TestEnum> w;
    ASSERT_EQUALS(E0, w.load());
    ASSERT_EQUALS(E0, testAtomicWordCompareAndSwap(w, E0, E1));
    ASSERT_EQUALS(E1, w.load());
    ASSERT_EQUALS(E1, testAtomicWordCompareAndSwap(w, E0, E2));
    ASSERT_EQUALS(E1, w.load());
}

TEST(AtomicWordTests, BasicOperationsUnsigned32Bit) {
    typedef unsigned WordType;
    testAtomicWordBasicOperations<AtomicWord<unsigned>>();
    testAtomicWordBitOperations<AtomicWord<unsigned>>();

    AtomicWord<unsigned> w(0xdeadbeef);
    ASSERT_EQUALS(WordType(0xdeadbeef), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeef),
                  testAtomicWordCompareAndSwap<WordType>(w, 0xdeadbeef, 0xcafe1234));
    ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
    ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsUnsigned64Bit) {
    typedef unsigned long long WordType;
    testAtomicWordBasicOperations<AtomicWord<unsigned long long>>();
    testAtomicWordBitOperations<AtomicWord<unsigned long long>>();

    AtomicWord<unsigned long long> w(0xdeadbeefcafe1234ULL);
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234ULL), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(
        WordType(0xdeadbeefcafe1234ULL),
        testAtomicWordCompareAndSwap<WordType>(w, 0xdeadbeefcafe1234ULL, 0xfedcba9876543210ULL));
    ASSERT_EQUALS(WordType(0xfedcba9876543210ULL), w.fetchAndAdd(0xf0000000ULL));
    ASSERT_EQUALS(WordType(0xfedcba9966543210ULL), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsSigned32Bit) {
    typedef int WordType;
    testAtomicWordBasicOperations<AtomicWord<int>>();

    AtomicWord<int> w(0xdeadbeef);
    ASSERT_EQUALS(WordType(0xdeadbeef), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(WordType(0xdeadbeef),
                  testAtomicWordCompareAndSwap<WordType>(w, 0xdeadbeef, 0xcafe1234));
    ASSERT_EQUALS(WordType(0xcafe1234), w.fetchAndAdd(0xf000));
    ASSERT_EQUALS(WordType(0xcaff0234), w.swap(0));
    ASSERT_EQUALS(WordType(0), w.load());
}

TEST(AtomicWordTests, BasicOperationsSigned64Bit) {
    typedef long long WordType;
    testAtomicWordBasicOperations<AtomicWord<long long>>();

    AtomicWord<long long> w(0xdeadbeefcafe1234ULL);
    ASSERT_EQUALS(WordType(0xdeadbeefcafe1234LL), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(
        WordType(0xdeadbeefcafe1234LL),
        testAtomicWordCompareAndSwap<WordType>(w, 0xdeadbeefcafe1234LL, 0xfedcba9876543210LL));
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

    ASSERT_EQUALS(WordType(2), testAtomicWordCompareAndSwap<WordType>(w, 0, 1));
    ASSERT_EQUALS(WordType(2), w.load());
    ASSERT_EQUALS(WordType(2), testAtomicWordCompareAndSwap<WordType>(w, 2, 1));
    ASSERT_EQUALS(WordType(1), w.load());

    w.store(15);
    ASSERT_EQUALS(WordType(15), testAtomicWordCompareAndSwap<WordType>(w, 15, 0));
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
