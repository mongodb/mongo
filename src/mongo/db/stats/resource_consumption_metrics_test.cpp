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

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

ServerParameter* getServerParameter(const std::string& name) {
    return ServerParameterSet::getNodeParameterSet()->get(name);
}
}  // namespace

class ResourceConsumptionMetricsTest : public LockerNoopServiceContextTest {
public:
    void setUp() {
        _opCtx = makeOperationContext();
        gAggregateOperationResourceConsumptionMetrics = true;
        gDocumentUnitSizeBytes = 128;
        gIndexEntryUnitSizeBytes = 16;

        auto svcCtx = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(svcCtx);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(svcCtx, std::move(replCoord));
    }

    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ResourceConsumptionMetricsTest, Merge) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());

    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    operationMetrics.beginScopedCollecting(_opCtx.get(), "db1");
    globalResourceConsumption.merge(
        _opCtx.get(), operationMetrics.getDbName(), operationMetrics.getMetrics());
    globalResourceConsumption.merge(
        _opCtx.get(), operationMetrics.getDbName(), operationMetrics.getMetrics());


    auto dbMetrics = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(dbMetrics.count("db1"), 1);
    ASSERT_EQ(dbMetrics.count("db2"), 0);
    ASSERT_EQ(dbMetrics.count("db3"), 0);
    operationMetrics.endScopedCollecting();

    operationMetrics.beginScopedCollecting(_opCtx.get(), "db2");
    globalResourceConsumption.merge(
        _opCtx.get(), operationMetrics.getDbName(), operationMetrics.getMetrics());
    globalResourceConsumption.merge(
        _opCtx.get(), operationMetrics.getDbName(), operationMetrics.getMetrics());

    dbMetrics = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(dbMetrics.count("db1"), 1);
    ASSERT_EQ(dbMetrics.count("db2"), 1);
    ASSERT_EQ(dbMetrics.count("db3"), 0);
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

    auto metricsCopy = globalResourceConsumption.getAndClearDbMetrics();
    ASSERT_EQ(metricsCopy.size(), 1);

    // Don't collect
    {
        const bool collectMetrics = false;
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1", collectMetrics);
        ASSERT_FALSE(operationMetrics.isCollecting());
    }

    ASSERT_FALSE(operationMetrics.isCollecting());

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);

    // Collect
    { ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1"); }

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);

    // Collect on a different database
    { ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db2"); }

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 1);
    ASSERT_EQ(metricsCopy.count("db2"), 1);

    // Ensure fetch and clear works.
    auto metrics = globalResourceConsumption.getAndClearDbMetrics();
    ASSERT_EQ(metrics.count("db1"), 1);
    ASSERT_EQ(metrics.count("db2"), 1);

    metricsCopy = globalResourceConsumption.getDbMetrics();
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

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
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

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy.count("db2"), 0);
    ASSERT_EQ(metricsCopy.count("db3"), 0);
    ASSERT_EQ(metricsCopy.count("db4"), 0);

    // Ensure fetch and clear works.
    auto metrics = globalResourceConsumption.getAndClearDbMetrics();
    ASSERT_EQ(metrics.count("db1"), 1);
    ASSERT_EQ(metrics.count("db2"), 0);

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy.count("db1"), 0);
    ASSERT_EQ(metricsCopy.count("db2"), 0);
}

namespace {
ResourceConsumption::DocumentUnitCounter makeDocUnits(size_t bytes) {
    ResourceConsumption::DocumentUnitCounter docUnitsReturned;
    docUnitsReturned.observeOne(bytes);
    return docUnitsReturned;
}
}  // namespace

TEST_F(ResourceConsumptionMetricsTest, IncrementReadMetrics) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 2);
        operationMetrics.incrementOneIdxEntryRead("", 8);
        operationMetrics.incrementKeysSorted(16);
        operationMetrics.incrementSorterSpills(32);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(64));
        operationMetrics.incrementOneCursorSeek("");
    }

    ASSERT(operationMetrics.hasCollectedMetrics());

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), 2);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), 8);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.keysSorted, 16);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.sorterSpills, 32);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.bytes(), 64);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, 1);

    // Clear metrics so we do not double-count.
    operationMetrics.reset();

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 32);
        operationMetrics.incrementOneIdxEntryRead("", 128);
        operationMetrics.incrementKeysSorted(256);
        operationMetrics.incrementSorterSpills(512);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(1024));
        operationMetrics.incrementOneCursorSeek("");
    }

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), 2);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.keysSorted, 16 + 256);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.sorterSpills, 32 + 512);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.bytes(), 64 + 1024);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, 1 + 1);
}

TEST_F(ResourceConsumptionMetricsTest, IncrementReadMetricsSecondary) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    ASSERT_OK(repl::ReplicationCoordinator::get(_opCtx.get())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 2);
        operationMetrics.incrementOneIdxEntryRead("", 8);
        operationMetrics.incrementKeysSorted(16);
        operationMetrics.incrementSorterSpills(32);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(64));
        operationMetrics.incrementOneCursorSeek("");
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.bytes(), 2);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.bytes(), 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.keysSorted, 16);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.sorterSpills, 32);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.bytes(), 64);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.cursorSeeks, 1);

    // Clear metrics so we do not double-count.
    operationMetrics.reset();

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 32);
        operationMetrics.incrementOneIdxEntryRead("", 128);
        operationMetrics.incrementKeysSorted(256);
        operationMetrics.incrementSorterSpills(512);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(1024));
        operationMetrics.incrementOneCursorSeek("");
    }

    metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.bytes(), 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.units(), 2);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.bytes(), 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.keysSorted, 16 + 256);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.sorterSpills, 32 + 512);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.bytes(), 64 + 1024);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.cursorSeeks, 1 + 1);
}

TEST_F(ResourceConsumptionMetricsTest, IncrementReadMetricsAcrossStates) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    // Start collecting metrics in the primary state, then change to secondary. Metrics should be
    // attributed to the secondary state, since that is the state where the operation completed.
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 2);
        operationMetrics.incrementOneIdxEntryRead("", 8);
        operationMetrics.incrementKeysSorted(16);
        operationMetrics.incrementSorterSpills(32);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(64));
        operationMetrics.incrementOneCursorSeek("");

        ASSERT_OK(repl::ReplicationCoordinator::get(_opCtx.get())
                      ->setFollowerMode(repl::MemberState::RS_SECONDARY));

        operationMetrics.incrementOneDocRead("", 32);
        operationMetrics.incrementOneIdxEntryRead("", 128);
        operationMetrics.incrementKeysSorted(256);
        operationMetrics.incrementSorterSpills(512);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(1024));
        operationMetrics.incrementOneCursorSeek("");
    }

    auto metricsCopy = globalResourceConsumption.getAndClearDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.keysSorted, 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.bytes(), 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.units(), 2);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.bytes(), 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.keysSorted, 16 + 256);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.sorterSpills, 32 + 512);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.bytes(), 64 + 1024);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.cursorSeeks, 1 + 1);

    operationMetrics.reset();

    // Start collecting metrics in the secondary state, then change to primary. Metrics should be
    // attributed to the primary state only.
    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        operationMetrics.incrementOneDocRead("", 2);
        operationMetrics.incrementOneIdxEntryRead("", 8);
        operationMetrics.incrementKeysSorted(16);
        operationMetrics.incrementSorterSpills(32);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(64));
        operationMetrics.incrementOneCursorSeek("");

        ASSERT_OK(repl::ReplicationCoordinator::get(_opCtx.get())
                      ->setFollowerMode(repl::MemberState::RS_PRIMARY));

        operationMetrics.incrementOneDocRead("", 32);
        operationMetrics.incrementOneIdxEntryRead("", 128);
        operationMetrics.incrementKeysSorted(256);
        operationMetrics.incrementSorterSpills(512);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(1024));
        operationMetrics.incrementOneCursorSeek("");
    }

    metricsCopy = globalResourceConsumption.getAndClearDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), 2 + 32);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), 2);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), 8 + 128);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.keysSorted, 16 + 256);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.sorterSpills, 32 + 512);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.bytes(), 64 + 1024);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.units(), 1 + 8);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, 1 + 1);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsRead.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.idxEntriesRead.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.keysSorted, 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.sorterSpills, 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.bytes(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.docsReturned.units(), 0);
    ASSERT_EQ(metricsCopy["db1"].secondaryReadMetrics.cursorSeeks, 0);
}

TEST_F(ResourceConsumptionMetricsTest, DocumentUnitsRead) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedBytes = 0;
    int expectedUnits = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        // Each of these should be counted as 1 document unit (unit size = 128).
        operationMetrics.incrementOneDocRead("", 2);
        operationMetrics.incrementOneDocRead("", 4);
        operationMetrics.incrementOneDocRead("", 8);
        operationMetrics.incrementOneDocRead("", 16);
        operationMetrics.incrementOneDocRead("", 32);
        operationMetrics.incrementOneDocRead("", 64);
        operationMetrics.incrementOneDocRead("", 128);
        expectedBytes += 2 + 4 + 8 + 16 + 32 + 64 + 128;
        expectedUnits += 7;

        // Each of these should be counted as 2 document units (unit size = 128).
        operationMetrics.incrementOneDocRead("", 129);
        operationMetrics.incrementOneDocRead("", 200);
        operationMetrics.incrementOneDocRead("", 255);
        operationMetrics.incrementOneDocRead("", 256);
        expectedBytes += 129 + 200 + 255 + 256;
        expectedUnits += 8;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), expectedBytes);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), expectedUnits);
}

TEST_F(ResourceConsumptionMetricsTest, DocumentUnitsWritten) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedBytes = 0;
    int expectedUnits = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        // Each of these should be counted as 1 document unit (unit size = 128).
        operationMetrics.incrementOneDocWritten("", 2);
        operationMetrics.incrementOneDocWritten("", 4);
        operationMetrics.incrementOneDocWritten("", 8);
        operationMetrics.incrementOneDocWritten("", 16);
        operationMetrics.incrementOneDocWritten("", 32);
        operationMetrics.incrementOneDocWritten("", 64);
        operationMetrics.incrementOneDocWritten("", 128);
        expectedBytes += 2 + 4 + 8 + 16 + 32 + 64 + 128;
        expectedUnits += 7;

        // Each of these should be counted as 2 document units (unit size = 128).
        operationMetrics.incrementOneDocWritten("", 129);
        operationMetrics.incrementOneDocWritten("", 200);
        operationMetrics.incrementOneDocWritten("", 255);
        operationMetrics.incrementOneDocWritten("", 256);
        expectedBytes += 129 + 200 + 255 + 256;
        expectedUnits += 8;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].writeMetrics.docsWritten.bytes(), expectedBytes);
    ASSERT_EQ(metricsCopy["db1"].writeMetrics.docsWritten.units(), expectedUnits);
}

TEST_F(ResourceConsumptionMetricsTest, TotalUnitsWritten) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedUnits = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        // Each of these should be counted as 1 total unit (unit size = 128).
        operationMetrics.incrementOneDocWritten("", 2);
        operationMetrics.incrementOneDocWritten("", 4);
        operationMetrics.incrementOneDocWritten("", 8);
        operationMetrics.incrementOneDocWritten("", 16);
        operationMetrics.incrementOneDocWritten("", 32);
        operationMetrics.incrementOneDocWritten("", 64);
        operationMetrics.incrementOneDocWritten("", 128);
        expectedUnits += 7;

        // Each of these should be counted as 2 total units (unit size = 128).
        operationMetrics.incrementOneDocWritten("", 129);
        operationMetrics.incrementOneDocWritten("", 200);
        operationMetrics.incrementOneDocWritten("", 255);
        operationMetrics.incrementOneDocWritten("", 256);
        expectedUnits += 8;

        // Each of these groups should be counted as 1 total unit, combining documents with index
        // bytes written.

        // Index writes prior to document write.
        operationMetrics.incrementOneDocWritten("", 0);
        operationMetrics.incrementOneIdxEntryWritten("", 2);
        operationMetrics.incrementOneDocWritten("", 5);
        expectedUnits += 1;

        // Index writes after document write.
        operationMetrics.incrementOneDocWritten("", 2);
        operationMetrics.incrementOneIdxEntryWritten("", 126);
        expectedUnits += 1;

        // No index writes.
        operationMetrics.incrementOneDocWritten("", 129);
        expectedUnits += 2;

        operationMetrics.incrementOneDocWritten("", 127);
        operationMetrics.incrementOneIdxEntryWritten("", 1);
        expectedUnits += 1;

        // Exceeds unit size and thus counts as 2 units.
        operationMetrics.incrementOneDocWritten("", 1);
        operationMetrics.incrementOneIdxEntryWritten("", 1);
        operationMetrics.incrementOneIdxEntryWritten("", 1);
        operationMetrics.incrementOneIdxEntryWritten("", 1);
        operationMetrics.incrementOneIdxEntryWritten("", 128);
        expectedUnits += 2;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].writeMetrics.totalWritten.units(), expectedUnits);
}

TEST_F(ResourceConsumptionMetricsTest, IdxEntryUnitsRead) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedBytes = 0;
    int expectedUnits = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        gIndexEntryUnitSizeBytes = 16;

        // Each of these should be counted as 1 document unit.
        operationMetrics.incrementOneIdxEntryRead("", 2);
        operationMetrics.incrementOneIdxEntryRead("", 4);
        operationMetrics.incrementOneIdxEntryRead("", 8);
        operationMetrics.incrementOneIdxEntryRead("", 16);
        expectedBytes += 2 + 4 + 8 + 16;
        expectedUnits += 4;

        // Each of these should be counted as 2 document unit.
        operationMetrics.incrementOneIdxEntryRead("", 17);
        operationMetrics.incrementOneIdxEntryRead("", 31);
        operationMetrics.incrementOneIdxEntryRead("", 32);
        expectedBytes += 17 + 31 + 32;
        expectedUnits += 6;

        gIndexEntryUnitSizeBytes = 32;

        // Each of these should be counted as 1 document unit.
        operationMetrics.incrementOneIdxEntryRead("", 17);
        operationMetrics.incrementOneIdxEntryRead("", 31);
        operationMetrics.incrementOneIdxEntryRead("", 32);
        expectedBytes += 17 + 31 + 32;
        expectedUnits += 3;

        // Each of these should be counted as 2 document units.
        operationMetrics.incrementOneIdxEntryRead("", 33);
        operationMetrics.incrementOneIdxEntryRead("", 63);
        operationMetrics.incrementOneIdxEntryRead("", 64);
        expectedBytes += 33 + 63 + 64;
        expectedUnits += 6;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), expectedBytes);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), expectedUnits);
}

TEST_F(ResourceConsumptionMetricsTest, IdxEntryUnitsWritten) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedBytes = 0;
    int expectedUnits = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");

        gIndexEntryUnitSizeBytes = 16;

        // Each of these should be counted as 1 document unit.
        operationMetrics.incrementOneIdxEntryWritten("", 2);
        operationMetrics.incrementOneIdxEntryWritten("", 4);
        operationMetrics.incrementOneIdxEntryWritten("", 8);
        operationMetrics.incrementOneIdxEntryWritten("", 16);
        expectedBytes += 2 + 4 + 8 + 16;
        expectedUnits += 4;

        // Each of these should be counted as 2 document units.
        operationMetrics.incrementOneIdxEntryWritten("", 17);
        operationMetrics.incrementOneIdxEntryWritten("", 31);
        operationMetrics.incrementOneIdxEntryWritten("", 32);
        expectedBytes += 17 + 31 + 32;
        expectedUnits += 6;

        gIndexEntryUnitSizeBytes = 32;

        // Each of these should be counted as 1 document unit.
        operationMetrics.incrementOneIdxEntryWritten("", 17);
        operationMetrics.incrementOneIdxEntryWritten("", 31);
        operationMetrics.incrementOneIdxEntryWritten("", 32);
        expectedBytes += 17 + 31 + 32;
        expectedUnits += 3;

        // Each of these should be counted as 2 document units.
        operationMetrics.incrementOneIdxEntryWritten("", 33);
        operationMetrics.incrementOneIdxEntryWritten("", 63);
        operationMetrics.incrementOneIdxEntryWritten("", 64);
        expectedBytes += 33 + 63 + 64;
        expectedUnits += 6;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].writeMetrics.idxEntriesWritten.bytes(), expectedBytes);
    ASSERT_EQ(metricsCopy["db1"].writeMetrics.idxEntriesWritten.units(), expectedUnits);
}

TEST_F(ResourceConsumptionMetricsTest, CpuNanos) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    // Do not run the test if a CPU timer is not available for this system.
    if (!OperationCPUTimer::get(_opCtx.get())) {
        return;
    }

    // Helper to busy wait.
    auto spinFor = [&](Milliseconds millis) {
        AtomicWord<bool> mayJoin{false};
        stdx::thread blocker([&] {
            sleepFor(millis);
            mayJoin.store(true);
        });
        while (!mayJoin.load()) {
            // Busy wait for the blocker thread.
        }
        blocker.join();
    };

    {
        // Ensure that the CPU timer increases relative to a single operation.
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");
        auto lastNanos = operationMetrics.getMetrics().cpuTimer->getElapsed();
        spinFor(Milliseconds(1));
        ASSERT_GT(operationMetrics.getMetrics().cpuTimer->getElapsed(), lastNanos);
    }

    // Ensure that the CPU timer stops counting past the end of the scope.
    auto nanos = operationMetrics.getMetrics().cpuTimer->getElapsed();
    spinFor(Milliseconds(1));
    ASSERT_EQ(nanos, operationMetrics.getMetrics().cpuTimer->getElapsed());

    // Ensure the CPU time gets aggregated globally.
    auto dbMetrics = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(dbMetrics["db1"].cpuNanos, nanos);

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");
        spinFor(Milliseconds(1));
    }

    // Ensure the aggregated CPU time increases over time.
    nanos += operationMetrics.getMetrics().cpuTimer->getElapsed();
    dbMetrics = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(dbMetrics["db1"].cpuNanos, nanos);

    // Ensure the CPU time is aggregated globally.
    auto globalCpuTime = globalResourceConsumption.getCpuTime();
    ASSERT_EQ(dbMetrics["db1"].cpuNanos, globalCpuTime);

    // Ensure the CPU time can be reset.
    globalResourceConsumption.getAndClearCpuTime();
    globalCpuTime = globalResourceConsumption.getCpuTime();
    ASSERT_EQ(Nanoseconds(0), globalCpuTime);
}

TEST_F(ResourceConsumptionMetricsTest, CursorSeeks) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    int expectedSeeks = 0;

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");
        operationMetrics.incrementOneCursorSeek("");
        operationMetrics.incrementOneCursorSeek("");
        operationMetrics.incrementOneCursorSeek("");

        expectedSeeks += 3;
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, expectedSeeks);
}

TEST_F(ResourceConsumptionMetricsTest, PauseMetricsCollectorBlock) {
    auto& globalResourceConsumption = ResourceConsumption::get(getServiceContext());
    auto& operationMetrics = ResourceConsumption::MetricsCollector::get(_opCtx.get());

    {
        ResourceConsumption::ScopedMetricsCollector scope(_opCtx.get(), "db1");
        {
            // Metrics increase within this scope should not be applied.
            ResourceConsumption::PauseMetricsCollectorBlock pauseMetricsCollection(_opCtx.get());
            operationMetrics.incrementOneDocRead("", 2);
            operationMetrics.incrementOneIdxEntryRead("", 8);
            operationMetrics.incrementKeysSorted(16);
            operationMetrics.incrementSorterSpills(32);
            operationMetrics.incrementDocUnitsReturned("", makeDocUnits(64));
            operationMetrics.incrementOneCursorSeek("");
        }

        // Once PauseMetricsCollectorBlock goes out of scope, metrics collection should work as
        // normal.
        operationMetrics.incrementOneDocRead("", 4);
        operationMetrics.incrementOneIdxEntryRead("", 16);
        operationMetrics.incrementKeysSorted(32);
        operationMetrics.incrementSorterSpills(64);
        operationMetrics.incrementDocUnitsReturned("", makeDocUnits(128));
        operationMetrics.incrementOneCursorSeek("");
    }

    auto metricsCopy = globalResourceConsumption.getDbMetrics();
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.bytes(), 4);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.bytes(), 16);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.idxEntriesRead.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.keysSorted, 32);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.sorterSpills, 64);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.bytes(), 128);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.docsReturned.units(), 1);
    ASSERT_EQ(metricsCopy["db1"].primaryReadMetrics.cursorSeeks, 1);
}

}  // namespace mongo
