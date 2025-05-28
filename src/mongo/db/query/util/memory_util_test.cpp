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

#include "mongo/db/query/util/memory_util.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

#include <cstdlib>

namespace mongo::memory_util {

bool operator==(const MemorySize& lhs, const MemorySize& rhs) {
    constexpr double kEpsilon = 1e-10;
    return std::abs(lhs.size - rhs.size) < kEpsilon && lhs.units == rhs.units;
}

TEST(MemorySizeTest, ParseUnitStringPercent) {
    ASSERT_TRUE(MemoryUnits::kPercent == parseUnitString("%"));
}

TEST(MemorySizeTest, ParseUnitStringBytes) {
    ASSERT_TRUE(MemoryUnits::kBytes == parseUnitString(""));
    ASSERT_TRUE(MemoryUnits::kBytes == parseUnitString("B"));
}

TEST(MemorySizeTest, ParseUnitStringKB) {
    ASSERT_TRUE(MemoryUnits::kKB == parseUnitString("KB"));
    ASSERT_TRUE(MemoryUnits::kKB == parseUnitString("kb"));
    ASSERT_TRUE(MemoryUnits::kKB == parseUnitString("kB"));
    ASSERT_TRUE(MemoryUnits::kKB == parseUnitString("Kb"));
}

TEST(MemorySizeTest, ParseUnitStringMB) {
    ASSERT_TRUE(MemoryUnits::kMB == parseUnitString("MB"));
    ASSERT_TRUE(MemoryUnits::kMB == parseUnitString("mb"));
    ASSERT_TRUE(MemoryUnits::kMB == parseUnitString("mB"));
    ASSERT_TRUE(MemoryUnits::kMB == parseUnitString("Mb"));
}

TEST(MemorySizeTest, ParseUnitStringGB) {
    ASSERT_TRUE(MemoryUnits::kGB == parseUnitString("GB"));
    ASSERT_TRUE(MemoryUnits::kGB == parseUnitString("gb"));
    ASSERT_TRUE(MemoryUnits::kGB == parseUnitString("gB"));
    ASSERT_TRUE(MemoryUnits::kGB == parseUnitString("Gb"));
}

TEST(MemorySizeTest, ParseUnitStringIncorrectValue) {
    ASSERT_NOT_OK(parseUnitString(" ").getStatus());
    ASSERT_NOT_OK(parseUnitString("a").getStatus());
    ASSERT_NOT_OK(parseUnitString("qb").getStatus());
    ASSERT_NOT_OK(parseUnitString("aqb").getStatus());
}

TEST(MemorySizeTest, ParseMemorySize) {
    ASSERT_TRUE((MemorySize{10.0, MemoryUnits::kPercent}) == MemorySize::parse("10%"));
    ASSERT_TRUE((MemorySize{150.0, MemoryUnits::kBytes}) == MemorySize::parse("150"));
    ASSERT_TRUE((MemorySize{150.0, MemoryUnits::kBytes}) == MemorySize::parse("150B"));
    ASSERT_TRUE((MemorySize{250.0, MemoryUnits::kKB}) == MemorySize::parse("250KB"));
    ASSERT_TRUE((MemorySize{300.0, MemoryUnits::kMB}) == MemorySize::parse("300MB"));
    ASSERT_TRUE((MemorySize{4.0, MemoryUnits::kGB}) == MemorySize::parse("4GB"));
    ASSERT_TRUE((MemorySize{5.1, MemoryUnits::kPercent}) == MemorySize::parse(" 5.1%"));
    ASSERT_TRUE((MemorySize{11.1, MemoryUnits::kMB}) == MemorySize::parse("11.1 mb"));
    ASSERT_TRUE((MemorySize{12.1, MemoryUnits::kGB}) == MemorySize::parse("  12.1    Gb  "));
}

TEST(MemorySizeTest, MemorySizeToBytes) {
    const auto cmp = [](auto value, auto str) {
        ASSERT_EQ(size_t(value), size_t(MemorySize::parse(str).getValue()));
    };
    cmp(123ll, "123");
    cmp(123ll, "123B");
    cmp(123ll * 1024, "123KB");
    cmp(123ll * 1024 * 1024, "123MB");
    cmp(123ll * 1024 * 1024 * 1024, "123GB");
}
}  // namespace mongo::memory_util
