/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class ResourceConsumptionMetricsTest : public ServiceContextTest {
public:
    void setUp() {
        _opCtx = makeOperationContext();
        gMeasureOperationResourceConsumption = true;
        gAggregateOperationResourceConsumptionMetrics = true;
    }

    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;
    ClientAndCtx makeClientAndCtx(const std::string& clientName) {
        auto client = getServiceContext()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        return std::make_pair(std::move(client), std::move(opCtx));
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ResourceConsumptionMetricsTest, Add) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());

    auto [client2, opCtx2] = makeClientAndCtx("opCtx2");

    auto& operationMetrics1 = ResourceConsumption::MetricsCollector::get(_opCtx.get());
    auto& operationMetrics2 = ResourceConsumption::MetricsCollector::get(opCtx2.get());

    operationMetrics1.beginScopedCollecting();
    operationMetrics1.setDbName("db1");
    operationMetrics2.beginScopedCollecting();
    operationMetrics2.setDbName("db2");
    globalResourceConsumption.add(operationMetrics1);
    globalResourceConsumption.add(operationMetrics1);
    globalResourceConsumption.add(operationMetrics2);
    globalResourceConsumption.add(operationMetrics2);

    auto globalMetrics = globalResourceConsumption.getMetrics();
    ASSERT_EQ(globalMetrics.count("db1"), 1);
    ASSERT_EQ(globalMetrics.count("db2"), 1);
    ASSERT_EQ(globalMetrics.count("db3"), 0);
}

TEST_F(ResourceConsumptionMetricsTest, ScopedMetricsCollector) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    // Collect, but don't set a dbName
    {
        const bool collectMetrics = true;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), collectMetrics);
        ASSERT_TRUE(operationMetrics.isCollecting());
    }

    ASSERT_FALSE(operationMetrics.isCollecting());

    auto metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.size(), 0);

    // Don't collect
    {
        const bool collectMetrics = false;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), collectMetrics);
        operationMetrics.setDbName("db1");
        ASSERT_FALSE(operationMetrics.isCollecting());
    }

    ASSERT_FALSE(operationMetrics.isCollecting());

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);

    // Collect
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get());
        operationMetrics.setDbName("db1");
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);

    // Collect on a different database
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get());
        operationMetrics.setDbName("db2");
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);
    ASSERT_EQ(metricsCopy.count("db2"), 1);

    // Ensure fetch and clear works.
    auto metrics = globalResourceConsumption.getAndClearMetrics();
    ASSERT_EQ(metrics.count("db1"), 1);
    ASSERT_EQ(metrics.count("db2"), 1);

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);
    ASSERT_EQ(metricsCopy.count("db2"), 0);
}

TEST_F(ResourceConsumptionMetricsTest, NestedScopedMetricsCollector) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    // Collect, nesting does not override that behavior.
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get());
        operationMetrics.setDbName("db1");

        {
            const bool collectMetrics = false;
            ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), collectMetrics);
            ASSERT_TRUE(operationMetrics.isCollecting());

            {
                ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get());
                ASSERT_TRUE(operationMetrics.isCollecting());
            }
        }
    }

    auto metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);

    // Don't collect, nesting does not override that behavior.
    {
        const bool collectMetrics = false;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), collectMetrics);
        operationMetrics.setDbName("db2");

        {
            ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get());
            ASSERT_FALSE(operationMetrics.isCollecting());

            {
                ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), collectMetrics);
                ASSERT_FALSE(operationMetrics.isCollecting());
            }
        }
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db2"), 0);

    // Ensure fetch and clear works.
    auto metrics = globalResourceConsumption.getAndClearMetrics();
    ASSERT_EQ(metrics.count("db1"), 1);
    ASSERT_EQ(metrics.count("db2"), 0);

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);
    ASSERT_EQ(metricsCopy.count("db2"), 0);
}
}  // namespace mongo
