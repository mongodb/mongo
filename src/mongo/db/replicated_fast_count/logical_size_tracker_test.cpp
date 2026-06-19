/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/logical_size_tracker.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

// Returns CollectionOptions marking a cold collection for the real WT storage engine.
CollectionOptions coldCollectionOptions() {
    CollectionOptions opts;
    opts.storageEngine =
        BSON("wiredTiger" << BSON("configString" << "disaggregated=(storage_tier=cold)"));
    return opts;
}

class CollectionMockWithSize : public CollectionMock {
public:
    CollectionMockWithSize(UUID uuid,
                           NamespaceString nss,
                           int64_t size,
                           CollectionOptions opts = {})
        : CollectionMock(uuid, nss), _size(size), _opts(std::move(opts)) {}

    long long dataSize(OperationContext*) const override {
        return _size;
    }

    const CollectionOptions& getCollectionOptions() const override {
        return _opts;
    }

private:
    int64_t _size;
    CollectionOptions _opts;
};

class LogicalSizeTrackerTest : public CatalogTestFixture {
protected:
    void registerMockCollection(NamespaceString nss,
                                UUID uuid,
                                int64_t size,
                                CollectionOptions opts = {}) {
        auto mock = std::make_shared<CollectionMockWithSize>(uuid, nss, size, std::move(opts));
        Lock::GlobalWrite lk(operationContext());
        CollectionCatalog::write(operationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(operationContext(), std::move(mock), boost::none);
        });
    }
};

TEST_F(LogicalSizeTrackerTest, SnapshotHotOnly) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colA"), UUID::gen(), 100);
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colB"), UUID::gen(), 200);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 300);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

TEST_F(LogicalSizeTrackerTest, SnapshotSumsCollectionsAcrossDatabases) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("db1", "colA"), UUID::gen(), 100);
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("db2", "colB"), UUID::gen(), 200);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 300);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

TEST_F(LogicalSizeTrackerTest, SnapshotColdOnly) {
    registerMockCollection(NamespaceString::createNamespaceString_forTest("test", "colA"),
                           UUID::gen(),
                           100,
                           coldCollectionOptions());
    registerMockCollection(NamespaceString::createNamespaceString_forTest("test", "colB"),
                           UUID::gen(),
                           200,
                           coldCollectionOptions());

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 0);
    ASSERT_EQ(snapshot.logicalBytesCold, 300);
}

TEST_F(LogicalSizeTrackerTest, SnapshotHotAndCold) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "hotCol"), UUID::gen(), 100);
    registerMockCollection(NamespaceString::createNamespaceString_forTest("test", "coldCol"),
                           UUID::gen(),
                           200,
                           coldCollectionOptions());

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 100);
    ASSERT_EQ(snapshot.logicalBytesCold, 200);
}

TEST_F(LogicalSizeTrackerTest, OplogWritesContributeToHotBytes) {
    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());
    const auto before = tracker.getLatestSnapshot();

    namespace th = replicated_fast_count::test_helpers;
    const th::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("logical_size_test", "collA"),
        .uuid = UUID::gen(),
    };
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));

    const auto entry1 = th::makeOplogEntry(Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10);
    const auto entry2 = th::makeOplogEntry(Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 20);

    const int64_t expectedOplogDelta =
        entry1.getEntry().toBSON().objsize() + entry2.getEntry().toBSON().objsize();

    // Important: writeToOplog() only appends records to local.oplog.rs.
    // It does not apply the oplog entries to collA or otherwise mutate collA's
    // CollectionCatalog-visible size.
    th::writeToOplog(operationContext(), entry1);
    th::writeToOplog(operationContext(), entry2);

    tracker.refreshLatestSnapshot_ForTest(operationContext());
    const auto after = tracker.getLatestSnapshot();

    // The oplog is hot by default, so only the hot bucket should change.
    ASSERT_EQ(after.logicalBytesCold, before.logicalBytesCold);
    ASSERT_EQ(after.logicalBytesHot - before.logicalBytesHot, expectedOplogDelta);
}

TEST_F(LogicalSizeTrackerTest, SnapshotIncludesTimeseriesBuckets) {
    CollectionOptions opts;
    opts.timeseries.emplace(TimeseriesOptions("time"));

    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "system.buckets.myts"),
        UUID::gen(),
        300,
        opts);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 300);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

TEST_F(LogicalSizeTrackerTest, EmptyCollectionsReportZeroSize) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colA"), UUID::gen(), 0);
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colB"), UUID::gen(), 0);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 0);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

TEST_F(LogicalSizeTrackerTest, NegativeSizeCollectionTreatedAsZero) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colA"), UUID::gen(), -100);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 0);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

TEST_F(LogicalSizeTrackerTest, NegativeSizeExcludedFromPositiveTotal) {
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "negative"), UUID::gen(), -50);
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "positive"), UUID::gen(), 100);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());

    const auto snapshot = tracker.getLatestSnapshot();
    ASSERT_EQ(snapshot.logicalBytesHot, 100);
    ASSERT_EQ(snapshot.logicalBytesCold, 0);
}

using LogicalSizeTrackerDeathTest = LogicalSizeTrackerTest;

DEATH_TEST_F(LogicalSizeTrackerDeathTest, DataSizeSumOverflowCrashes, "12894100") {
    registerMockCollection(NamespaceString::createNamespaceString_forTest("test", "colA"),
                           UUID::gen(),
                           std::numeric_limits<long long>::max());
    registerMockCollection(
        NamespaceString::createNamespaceString_forTest("test", "colB"), UUID::gen(), 1);

    LogicalSizeTracker tracker;
    tracker.refreshLatestSnapshot_ForTest(operationContext());
}

}  // namespace
}  // namespace mongo
