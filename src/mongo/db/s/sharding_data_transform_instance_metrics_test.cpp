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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/sharding_data_transform_metrics_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class InstanceMetricsWithObserverMock {
public:
    InstanceMetricsWithObserverMock(int64_t startTime,
                                    int64_t timeRemaining,
                                    ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
        : _impl{UUID::gen(),
                NamespaceString("test.source"),
                ShardingDataTransformInstanceMetrics::Role::kDonor,
                BSON("command"
                     << "test"),
                cumulativeMetrics,
                std::make_unique<ObserverMock>(startTime, timeRemaining)} {}


private:
    ShardingDataTransformInstanceMetrics _impl;
};

class ShardingDataTransformInstanceMetricsTest : public ShardingDataTransformMetricsTestFixture {
public:
    std::unique_ptr<ShardingDataTransformInstanceMetrics> createInstanceMetrics(
        UUID instanceId = UUID::gen(), Role role = Role::kDonor) {
        return std::make_unique<ShardingDataTransformInstanceMetrics>(
            instanceId, kTestCommand, kTestNamespace, role, &_cumulativeMetrics);
    }

    std::unique_ptr<ShardingDataTransformInstanceMetrics> createInstanceMetrics(
        std::unique_ptr<ObserverMock> mock) {
        return std::make_unique<ShardingDataTransformInstanceMetrics>(
            UUID::gen(),
            kTestNamespace,
            ShardingDataTransformInstanceMetrics::Role::kDonor,
            kTestCommand,
            &_cumulativeMetrics,
            std::move(mock));
    }
};

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetrics) {
    for (auto i = 0; i < 100; i++) {
        auto metrics = createInstanceMetrics();
        ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 1);
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RegisterAndDeregisterMetricsAtOnce) {
    {
        std::vector<std::unique_ptr<ShardingDataTransformInstanceMetrics>> registered;
        for (auto i = 0; i < 100; i++) {
            registered.emplace_back(createInstanceMetrics());
            ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), registered.size());
        }
    }
    ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), 0);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperations) {
    doRandomOperationsTest<InstanceMetricsWithObserverMock>();
}

TEST_F(ShardingDataTransformInstanceMetricsTest, RandomOperationsMultithreaded) {
    doRandomOperationsMultithreadedTest<InstanceMetricsWithObserverMock>();
}


TEST_F(ShardingDataTransformInstanceMetricsTest, ReportForCurrentOpShouldHaveGenericDescription) {


    std::vector<Role> roles{Role::kCoordinator, Role::kDonor, Role::kRecipient};

    std::for_each(roles.begin(), roles.end(), [&](auto role) {
        auto instanceId = UUID::gen();
        auto metrics = createInstanceMetrics(instanceId, role);
        auto report = metrics->reportForCurrentOp();
        ASSERT_EQ(report.getStringField("desc").toString(),
                  fmt::format("ShardingDataTransformMetrics{}Service {}",
                              ShardingDataTransformMetrics::getRoleName(role),
                              instanceId.toString()));
    });
}

TEST_F(ShardingDataTransformInstanceMetricsTest, GetRoleNameShouldReturnCorrectName) {
    std::vector<std::pair<Role, std::string>> roles{
        {Role::kCoordinator, "Coordinator"},
        {Role::kDonor, "Donor"},
        {Role::kRecipient, "Recipient"},
    };

    std::for_each(roles.begin(), roles.end(), [&](auto role) {
        ASSERT_EQ(ShardingDataTransformMetrics::getRoleName(role.first), role.second);
    });
}


TEST_F(ShardingDataTransformInstanceMetricsTest, OnInsertAppliedShouldIncrementInsertsApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 0);
    metrics->onInsertApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("insertsApplied"), 1);
}


TEST_F(ShardingDataTransformInstanceMetricsTest, OnUpdateAppliedShouldIncrementUpdatesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 0);
    metrics->onUpdateApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("updatesApplied"), 1);
}

TEST_F(ShardingDataTransformInstanceMetricsTest, OnDeleteAppliedShouldIncrementDeletesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 0);
    metrics->onDeleteApplied();

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("deletesApplied"), 1);
}


TEST_F(ShardingDataTransformInstanceMetricsTest,
       OnOplogsEntriesAppliedShouldIncrementOplogsEntriesApplied) {
    auto metrics = createInstanceMetrics(UUID::gen(), Role::kRecipient);

    auto report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 0);
    metrics->onOplogEntriesApplied(100);

    report = metrics->reportForCurrentOp();
    ASSERT_EQ(report.getIntField("oplogEntriesApplied"), 100);
}

}  // namespace
}  // namespace mongo
