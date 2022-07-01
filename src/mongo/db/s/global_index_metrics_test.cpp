/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/global_index_metrics.h"
#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {


class GlobalIndexMetricsTest : public ShardingDataTransformMetricsTestFixture {

public:
    std::unique_ptr<GlobalIndexMetrics> createInstanceMetrics(ClockSource* clockSource,
                                                              UUID instanceId = UUID::gen(),
                                                              Role role = Role::kDonor) {
        return std::make_unique<GlobalIndexMetrics>(instanceId,
                                                    kTestCommand,
                                                    kTestNamespace,
                                                    role,
                                                    clockSource->now(),
                                                    clockSource,
                                                    &_cumulativeMetrics);
    }
};

TEST_F(GlobalIndexMetricsTest, ReportForCurrentOpShouldHaveGlobalIndexDescription) {
    std::vector<Role> roles{Role::kCoordinator, Role::kDonor, Role::kRecipient};

    std::for_each(roles.begin(), roles.end(), [&](Role role) {
        auto instanceId = UUID::gen();
        auto metrics = createInstanceMetrics(getClockSource(), instanceId, role);
        auto report = metrics->reportForCurrentOp();

        ASSERT_EQ(report.getStringField("desc").toString(),
                  fmt::format("GlobalIndexMetrics{}Service {}",
                              ShardingDataTransformMetrics::getRoleName(role),
                              instanceId.toString()));
    });
}
}  // namespace
}  // namespace mongo
