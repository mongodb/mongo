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

#include "mongo/db/repl/replication_coordinator_mock.h"
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

        auto svcCtx = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(svcCtx);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(svcCtx, std::move(replCoord));
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

    operationMetrics1.beginScopedCollecting("db1");
    operationMetrics2.beginScopedCollecting("db2");
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

    // Collect
    {
        const bool collectMetrics = true;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1", collectMetrics);
        ASSERT_TRUE(operationMetrics.isCollecting());
    }

    ASSERT_FALSE(operationMetrics.isCollecting());

    auto metricsCopy = globalResourceConsumption.getAndClearMetrics();
    ASSERT_EQ(metricsCopy.size(), 1);

    // Don't collect
    {
        const bool collectMetrics = false;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1", collectMetrics);
        ASSERT_FALSE(operationMetrics.isCollecting());
    }

    ASSERT_FALSE(operationMetrics.isCollecting());

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);

    // Collect
    { ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1"); }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);

    // Collect on a different database
    { ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db2"); }

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

    // Collect, nesting does not override that behavior or change the collection database.
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        ASSERT(operationMetrics.hasCollectedMetrics());
        {
            const bool collectMetrics = false;
            ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db2", collectMetrics);
            ASSERT_TRUE(operationMetrics.isCollecting());

            {
                ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db3");
                ASSERT_TRUE(operationMetrics.isCollecting());
            }
        }
    }

    auto metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);
    ASSERT_EQ(metricsCopy.count("db2"), 0);
    ASSERT_EQ(metricsCopy.count("db3"), 0);

    operationMetrics.reset();

    // Don't collect, nesting does not override that behavior.
    {
        const bool collectMetrics = false;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db2", collectMetrics);

        ASSERT_FALSE(operationMetrics.hasCollectedMetrics());

        {
            ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db3");
            ASSERT_FALSE(operationMetrics.isCollecting());

            {
                ResourceConsumption::ScopedMetricsCollector scope(
                    _opCtx.get(), "db4", collectMetrics);
                ASSERT_FALSE(operationMetrics.isCollecting());
            }
        }
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db2"), 0);
    ASSERT_EQ(metricsCopy.count("db3"), 0);
    ASSERT_EQ(metricsCopy.count("db4"), 0);

    // Ensure fetch and clear works.
    auto metrics = globalResourceConsumption.getAndClearMetrics();
    ASSERT_EQ(metrics.count("db1"), 1);
    ASSERT_EQ(metrics.count("db2"), 0);

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);
    ASSERT_EQ(metricsCopy.count("db2"), 0);
}

TEST_F(ResourceConsumptionMetricsTest, IncrementReadMetrics) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementDocBytesRead(_opCtx.get(), 2);
        operationMetrics.incrementDocUnitsRead(_opCtx.get(), 4);
        operationMetrics.incrementIdxEntriesRead(_opCtx.get(), 8);
        operationMetrics.incrementKeysSorted(_opCtx.get(), 16);
    }

    ASSERT(operationMetrics.hasCollectedMetrics());

    auto metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.docBytesRead, 2);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.docUnitsRead, 4);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.idxEntriesRead, 8);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.keysSorted, 16);

    // Clear metrics so we do not double-count.
    operationMetrics.reset();

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementDocBytesRead(_opCtx.get(), 32);
        operationMetrics.incrementDocUnitsRead(_opCtx.get(), 64);
        operationMetrics.incrementIdxEntriesRead(_opCtx.get(), 128);
        operationMetrics.incrementKeysSorted(_opCtx.get(), 256);
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.docBytesRead, 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.docUnitsRead, 4 + 64);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.idxEntriesRead, 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].primaryMetrics.keysSorted, 16 + 256);
}

TEST_F(ResourceConsumptionMetricsTest, IncrementReadMetricsSecondary) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    ASSERT_OK(repl::ReplicationCoordinator::get(_opCtx.get())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementDocBytesRead(_opCtx.get(), 2);
        operationMetrics.incrementDocUnitsRead(_opCtx.get(), 4);
        operationMetrics.incrementIdxEntriesRead(_opCtx.get(), 8);
        operationMetrics.incrementKeysSorted(_opCtx.get(), 16);
    }

    auto metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.docBytesRead, 2);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.docUnitsRead, 4);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.idxEntriesRead, 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.keysSorted, 16);

    // Clear metrics so we do not double-count.
    operationMetrics.reset();

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementDocBytesRead(_opCtx.get(), 32);
        operationMetrics.incrementDocUnitsRead(_opCtx.get(), 64);
        operationMetrics.incrementIdxEntriesRead(_opCtx.get(), 128);
        operationMetrics.incrementKeysSorted(_opCtx.get(), 256);
    }

    metricsCopy = globalResourceConsumption.getMetrics();
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.docBytesRead, 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.docUnitsRead, 4 + 64);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.idxEntriesRead, 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].secondaryMetrics.keysSorted, 16 + 256);
}

}  // namespace mongo
