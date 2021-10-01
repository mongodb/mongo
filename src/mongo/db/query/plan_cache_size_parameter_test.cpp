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

#include "mongo/db/query/plan_cache_size_parameter.h"

#include "mongo/unittest/unittest.h"

namespace mongo::plan_cache_util {

bool operator==(const PlanCacheSizeParameter& lhs, const PlanCacheSizeParameter& rhs) {
    constexpr double kEpsilon = 1e-10;
    return std::abs(lhs.size - rhs.size) < kEpsilon && lhs.units == rhs.units;
}

TEST(PlanCacheParameterTest, ParseUnitStringPercent) {
    ASSERT_TRUE(PlanCacheSizeUnits::kPercent == parseUnitString("%"));
}

TEST(PlanCacheParameterTest, ParseUnitStringMB) {
    ASSERT_TRUE(PlanCacheSizeUnits::kMB == parseUnitString("MB"));
    ASSERT_TRUE(PlanCacheSizeUnits::kMB == parseUnitString("mb"));
    ASSERT_TRUE(PlanCacheSizeUnits::kMB == parseUnitString("mB"));
    ASSERT_TRUE(PlanCacheSizeUnits::kMB == parseUnitString("Mb"));
}

TEST(PlanCacheParameterTest, ParseUnitStringGB) {
    ASSERT_TRUE(PlanCacheSizeUnits::kGB == parseUnitString("GB"));
    ASSERT_TRUE(PlanCacheSizeUnits::kGB == parseUnitString("gb"));
    ASSERT_TRUE(PlanCacheSizeUnits::kGB == parseUnitString("gB"));
    ASSERT_TRUE(PlanCacheSizeUnits::kGB == parseUnitString("Gb"));
}

TEST(PlanCacheParameterTest, ParseUnitStringIncorrectValue) {
    ASSERT_NOT_OK(parseUnitString("").getStatus());
    ASSERT_NOT_OK(parseUnitString(" ").getStatus());
    ASSERT_NOT_OK(parseUnitString("KB").getStatus());
}

TEST(PlanCacheParameterTest, ParsePlanCacheSizeParameter) {
    ASSERT_TRUE((PlanCacheSizeParameter{10.0, PlanCacheSizeUnits::kPercent}) ==
                PlanCacheSizeParameter::parse("10%"));
    ASSERT_TRUE((PlanCacheSizeParameter{300.0, PlanCacheSizeUnits::kMB}) ==
                PlanCacheSizeParameter::parse("300MB"));
    ASSERT_TRUE((PlanCacheSizeParameter{4.0, PlanCacheSizeUnits::kGB}) ==
                PlanCacheSizeParameter::parse("4GB"));
    ASSERT_TRUE((PlanCacheSizeParameter{5.1, PlanCacheSizeUnits::kPercent}) ==
                PlanCacheSizeParameter::parse(" 5.1%"));
    ASSERT_TRUE((PlanCacheSizeParameter{11.1, PlanCacheSizeUnits::kMB}) ==
                PlanCacheSizeParameter::parse("11.1 mb"));
    ASSERT_TRUE((PlanCacheSizeParameter{12.1, PlanCacheSizeUnits::kGB}) ==
                PlanCacheSizeParameter::parse("  12.1    Gb  "));
}
}  // namespace mongo::plan_cache_util
