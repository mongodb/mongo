// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/overflow_arithmetic.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <memory>

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
auto test(F f, A a, B b, std::type_identity_t<T> r) {
    return runTest<T>(false, f, a, b, r);
};

// Expect `f(a,b)` overflows.
template <typename T, typename F, typename A, typename B>
auto testOflow(F f, A a, B b) {
    return runTest<T>(true, f, a, b);
};

// Polymorphic lambdas to defer overload resolution until execution time.
constexpr auto polyMul = [](auto&&... a) {
    return overflow::mul(a...);
};
constexpr auto polyAdd = [](auto&&... a) {
    return overflow::add(a...);
};
constexpr auto polySub = [](auto&&... a) {
    return overflow::sub(a...);
};

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

TEST(OverflowArithmetic, SafeModTests) {
    // Mod -1 should not overflow for LLONG_MIN or INT_MIN.
    auto minLong = std::numeric_limits<long long>::min();
    auto minInt = std::numeric_limits<int>::min();
    ASSERT_EQ(overflow::safeMod(minLong, -1LL), 0);
    ASSERT_EQ(overflow::safeMod(minInt, -1), 0);

    // A divisor of 0 throws a user assertion.
    ASSERT_THROWS_CODE(overflow::safeMod(minLong, 0LL), AssertionException, 51259);
    ASSERT_THROWS_CODE(overflow::safeMod(minInt, 0), AssertionException, 51259);
}

TEST(OverflowArithmetic, HeterogeneousArguments) {
    {
        int r;
        ASSERT_FALSE(overflow::mul(1L, 2ULL, &r));
        ASSERT_EQ(r, 2);
    }
    {
        unsigned long long r;
        ASSERT_TRUE(overflow::mul(-1, 2, &r));
    }
}

}  // namespace
}  // namespace mongo
