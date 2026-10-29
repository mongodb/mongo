// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/memory_util.h"

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
