// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_framework_serialization.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::query_settings::query_framework {

TEST(QueryFrameworkSerialization, TestSerialization) {
    ASSERT_EQ(serialize(QueryFrameworkControlEnum::kTrySbeEngine), kSbe);
    ASSERT_EQ(serialize(QueryFrameworkControlEnum::kForceClassicEngine), kClassic);
}

TEST(QueryFrameworkSerialization, TestDeserialization) {
    ASSERT_EQ(parse(kSbe), QueryFrameworkControlEnum::kTrySbeEngine);
    ASSERT_EQ(parse(kClassic), QueryFrameworkControlEnum::kForceClassicEngine);
    ASSERT_THROWS_CODE(parse("cqf"), DBException, ErrorCodes::BadValue);
}
}  // namespace mongo::query_settings::query_framework
