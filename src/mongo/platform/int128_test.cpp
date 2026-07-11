// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/int128.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

#include <absl/numeric/int128.h>

using namespace mongo;

// Asserts that we can cast to uint128_t and cast back as well that the expected high and low bits
// after cast are following 2's complement.
void assertCastIsValid(int128_t val, uint64_t expectedHi, uint64_t expectedLo) {
    uint128_t castedInt = static_cast<uint128_t>(val);
    ASSERT_EQUALS(absl::Uint128High64(castedInt), expectedHi);
    ASSERT_EQUALS(absl::Uint128Low64(castedInt), expectedLo);
    int128_t backToSigned = static_cast<int128_t>(castedInt);
    ASSERT_EQUALS(val, backToSigned);
}

TEST(Int128, TestCastingPositive) {
    assertCastIsValid(12345, 0, 12345);
}

TEST(Int128, TestCastingNegative) {
    assertCastIsValid(-12345, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFCFC7);
}

TEST(Int128, MaxPositiveInt) {
    assertCastIsValid(std::numeric_limits<int128_t>::max(), 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF);
}

TEST(Int128, MaxNegativeInt) {
    assertCastIsValid(std::numeric_limits<int128_t>::min(), 0x8000000000000000, 0);
}

TEST(Int128, Multiplication) {
    // Verify that we can compile and execute int128 multiplication, sanitizer builds require
    // special link flags. See: https://bugs.llvm.org/show_bug.cgi?id=16404
    int128_t a = 5;
    int128_t b = 10;
    ASSERT_EQUALS(a * b, 50);
}
