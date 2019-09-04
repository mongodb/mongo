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

#include "mongo/platform/basic.h"

#include <limits>

#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

template <typename T>
constexpr T kMin = std::numeric_limits<T>::min();
template <typename T>
constexpr T kMax = std::numeric_limits<T>::max();

template <typename T, typename F, typename A, typename B>
bool runTest(bool oflow, F f, A a, B b, T r = {}) {
    T result;
    bool oflowed = f(a, b, &result);
    if (oflowed != oflow)
        return false;
    if (oflow)
        return true;
    return result == r;
}

// Expect `f(a,b) == r`.
template <typename T, typename F, typename A, typename B>
auto test(F f, A a, B b, stdx::type_identity_t<T> r) {
    return runTest<T>(false, f, a, b, r);
};

// Expect `f(a,b)` overflows.
template <typename T, typename F, typename A, typename B>
auto testOflow(F f, A a, B b) {
    return runTest<T>(true, f, a, b);
};

// Polymorphic lambdas to defer overload resolution until execution time.
constexpr auto polyMul = [](auto&&... a) { return overflow::mul(a...); };
constexpr auto polyAdd = [](auto&&... a) { return overflow::add(a...); };
constexpr auto polySub = [](auto&&... a) { return overflow::sub(a...); };

TEST(OverflowArithmetic, SignedMultiplicationTests) {
    using T = int64_t;
    static constexpr auto f = polyMul;
    ASSERT(test<T>(f, 0, kMax<T>, 0));
    ASSERT(test<T>(f, 0, kMin<T>, 0));
    ASSERT(test<T>(f, 1, kMax<T>, kMax<T>));
    ASSERT(test<T>(f, 1, kMin<T>, kMin<T>));
    ASSERT(test<T>(f, -1, kMax<T>, kMin<T> + 1));
    ASSERT(test<T>(f, 1000, 57, 57000));
    ASSERT(test<T>(f, 1000, -57, -57000));
    ASSERT(test<T>(f, -1000, -57, 57000));
    ASSERT(test<T>(f, 0x3fffffffffffffff, 2, 0x7ffffffffffffffe));
    ASSERT(test<T>(f, 0x3fffffffffffffff, -2, -0x7ffffffffffffffe));
    ASSERT(test<T>(f, -0x3fffffffffffffff, -2, 0x7ffffffffffffffe));
    ASSERT(testOflow<T>(f, -1, kMin<T>));
    ASSERT(testOflow<T>(f, 2, kMax<T>));
    ASSERT(testOflow<T>(f, -2, kMax<T>));
    ASSERT(testOflow<T>(f, 2, kMin<T>));
    ASSERT(testOflow<T>(f, -2, kMin<T>));
    ASSERT(testOflow<T>(f, kMin<T>, kMax<T>));
    ASSERT(testOflow<T>(f, kMax<T>, kMax<T>));
    ASSERT(testOflow<T>(f, kMin<T>, kMin<T>));
    ASSERT(testOflow<T>(f, 1LL << 62, 8));
    ASSERT(testOflow<T>(f, -(1LL << 62), 8));
    ASSERT(testOflow<T>(f, -(1LL << 62), -8));
}

TEST(OverflowArithmetic, UnignedMultiplicationTests) {
    using T = uint64_t;
    static constexpr auto f = polyMul;
    ASSERT(test<T>(f, 0, kMax<T>, 0));
    ASSERT(test<T>(f, 1, kMax<T>, kMax<T>));
    ASSERT(test<T>(f, 1000, 57, 57000));
    ASSERT(test<T>(f, 0x3fffffffffffffff, 2, 0x7ffffffffffffffe));
    ASSERT(test<T>(f, 0x7fffffffffffffff, 2, 0xfffffffffffffffe));
    ASSERT(testOflow<T>(f, 2, kMax<T>));
    ASSERT(testOflow<T>(f, kMax<T>, kMax<T>));
    ASSERT(testOflow<T>(f, 1LL << 62, 8));
    ASSERT(testOflow<T>(f, 0x7fffffffffffffff, 4));
}

TEST(OverflowArithmetic, SignedAdditionTests) {
    using T = int64_t;
    static constexpr auto f = polyAdd;
    ASSERT(test<T>(f, 0, kMax<T>, kMax<T>));
    ASSERT(test<T>(f, -1, kMax<T>, kMax<T> - 1));
    ASSERT(test<T>(f, 1, kMax<T> - 1, kMax<T>));
    ASSERT(test<T>(f, 0, kMin<T>, kMin<T>));
    ASSERT(test<T>(f, 1, kMin<T>, kMin<T> + 1));
    ASSERT(test<T>(f, -1, kMin<T> + 1, kMin<T>));
    ASSERT(test<T>(f, kMax<T>, kMin<T>, -1));
    ASSERT(test<T>(f, 1, 1, 2));
    ASSERT(test<T>(f, -1, -1, -2));
    ASSERT(testOflow<T>(f, kMax<T>, 1));
    ASSERT(testOflow<T>(f, kMax<T>, kMax<T>));
    ASSERT(testOflow<T>(f, kMin<T>, -1));
    ASSERT(testOflow<T>(f, kMin<T>, kMin<T>));
}

TEST(OverflowArithmetic, UnsignedAdditionTests) {
    using T = uint64_t;
    static constexpr auto f = polyAdd;
    ASSERT(test<T>(f, 0, kMax<T>, kMax<T>));
    ASSERT(test<T>(f, 1, kMax<T> - 1, kMax<T>));
    ASSERT(test<T>(f, 1, 1, 2));
    ASSERT(testOflow<T>(f, kMax<T>, 1));
    ASSERT(testOflow<T>(f, kMax<T>, kMax<T>));
}

TEST(OverflowArithmetic, SignedSubtractionTests) {
    using T = int64_t;
    static constexpr auto f = polySub;
    ASSERT(test<T>(f, kMax<T>, 0, kMax<T>));
    ASSERT(test<T>(f, kMax<T>, 1, kMax<T> - 1));
    ASSERT(test<T>(f, kMax<T> - 1, -1, kMax<T>));
    ASSERT(test<T>(f, kMin<T>, 0, kMin<T>));
    ASSERT(test<T>(f, kMin<T>, -1, kMin<T> + 1));
    ASSERT(test<T>(f, kMin<T> + 1, 1, kMin<T>));
    ASSERT(test<T>(f, kMax<T>, kMax<T>, 0));
    ASSERT(test<T>(f, kMin<T>, kMin<T>, 0));
    ASSERT(test<T>(f, 0, 0, 0));
    ASSERT(test<T>(f, 1, 1, 0));
    ASSERT(test<T>(f, 0, 1, -1));
    ASSERT(testOflow<T>(f, 0, kMin<T>));
    ASSERT(testOflow<T>(f, kMax<T>, -1));
    ASSERT(testOflow<T>(f, kMax<T>, kMin<T>));
    ASSERT(testOflow<T>(f, kMin<T>, 1));
    ASSERT(testOflow<T>(f, kMin<T>, kMax<T>));
}

TEST(OverflowArithmetic, UnsignedSubtractionTests) {
    using T = uint64_t;
    static constexpr auto f = polySub;
    ASSERT(test<T>(f, kMax<T>, 0, kMax<T>));
    ASSERT(test<T>(f, kMax<T>, 1, kMax<T> - 1));
    ASSERT(test<T>(f, kMax<T>, kMax<T>, 0));
    ASSERT(test<T>(f, 0, 0, 0));
    ASSERT(test<T>(f, 1, 1, 0));
    ASSERT(testOflow<T>(f, 0, 1));
    ASSERT(testOflow<T>(f, 0, kMax<T>));
}

TEST(OverflowArithmetic, HeterogeneousArguments) {
    {
        int r;
        ASSERT_FALSE(overflow::mul(long{1}, (unsigned long long){2}, &r));
        ASSERT_EQ(r, 2);
    }
    {
        unsigned long long r;
        ASSERT_TRUE(overflow::mul(-1, 2, &r));
    }
}

}  // namespace
}  // namespace mongo
