/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/util/bitset_compare.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
template <size_t N>
void assertLess(std::string_view lhs, std::string_view rhs) {
    std::bitset<N> lhb{lhs.data(), lhs.size()};
    std::bitset<N> lhb2{lhs.data(), lhs.size()};
    std::bitset<N> rhb{rhs.data(), rhs.size()};
    ASSERT_TRUE(bitsetLess(lhb, rhb));
    ASSERT_FALSE(bitsetLess(rhb, lhb));
    ASSERT_FALSE(bitsetLess(lhb, lhb));
    ASSERT_FALSE(bitsetLess(lhb, lhb2));
}
}  // namespace

TEST(BitsetCompareTest, Less) {
    assertLess<8>("0", "1");
    assertLess<32>("0", "1");
    assertLess<64>("0", "1");
    assertLess<100>("0", "1");


    assertLess<8>("10", "11");
    assertLess<32>("10", "11");
    assertLess<64>("10", "11");
    assertLess<100>("10", "11");

    assertLess<32>("1000000000010", "1000000000011");
    assertLess<64>("1000000000010", "1000000000011");
    assertLess<100>("1000000000010", "1000000000011");
}
}  // namespace mongo
