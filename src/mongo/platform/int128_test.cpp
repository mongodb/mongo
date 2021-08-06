/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/int128.h"
#include "mongo/unittest/unittest.h"

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
