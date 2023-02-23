/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/metrics/sharding_data_transform_metrics_macros.h"

#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ShardingDataTransformMetricsMacrosTest, IdlEnumSizeReshardingCoordinator) {
    ASSERT_EQ(IDL_ENUM_SIZE(CoordinatorStateEnum), kNumCoordinatorStateEnum);
}

TEST(ShardingDataTransformMetricsMacrosTest, IdlEnumSizeReshardingRecipient) {
    ASSERT_EQ(IDL_ENUM_SIZE(RecipientStateEnum), kNumRecipientStateEnum);
}

DEFINE_IDL_ENUM_SIZE_TEMPLATE_HELPER(testOne, CoordinatorStateEnum)

TEST(ShardingDataTransformMetricsMacrosTest, EnumSizeTemplateHelperWithOneEnum) {
    ASSERT_EQ(testOneEnumSizeTemplateHelper<CoordinatorStateEnum>::getSize(),
              kNumCoordinatorStateEnum);
}

DEFINE_IDL_ENUM_SIZE_TEMPLATE_HELPER(testMany,
                                     CoordinatorStateEnum,
                                     RecipientStateEnum,
                                     DonorStateEnum)

TEST(ShardingDataTransformMetricsMacrosTest, EnumSizeTemplateHelperWithManyEnumsCoordinator) {
    ASSERT_EQ(testManyEnumSizeTemplateHelper<CoordinatorStateEnum>::getSize(),
              kNumCoordinatorStateEnum);
}

TEST(ShardingDataTransformMetricsMacrosTest, EnumSizeTemplateHelperWithManyEnumsDonor) {
    ASSERT_EQ(testManyEnumSizeTemplateHelper<RecipientStateEnum>::getSize(),
              kNumRecipientStateEnum);
}

TEST(ShardingDataTransformMetricsMacrosTest, EnumSizeTemplateHelperWithManyEnumsRecipient) {
    ASSERT_EQ(testManyEnumSizeTemplateHelper<DonorStateEnum>::getSize(), kNumDonorStateEnum);
}

}  // namespace
}  // namespace mongo
