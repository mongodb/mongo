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

#include "mongo/db/ttl/ttl.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds/index_build_entry_helpers.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/durable_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/timeseries/timeseries_test_util.h"
#include "mongo/db/ttl/ttl_monitor.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <thread>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

// Must exist in the mongo namespace to be a friend class of the TTLMonitor.
class TTLTest : public service_context_test::WithSetupTransportLayer,
                public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        std::unique_ptr<TTLMonitor> ttlMonitor = std::make_unique<TTLMonitor>();
        TTLMonitor::set(service, std::move(ttlMonitor));
        startTTLMonitor(service, true);

        _opCtx = cc().makeOperationContext();

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));
        repl::createOplog(_opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        indexbuildentryhelpers::ensureIndexBuildEntriesNamespaceExists(_opCtx.get());
    }

    void tearDown() override {
        ServiceContextMongoDTest::tearDown();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void doTTLPassForTest(Date_t now) {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
        ttlMonitor->_doTTLPass(_opCtx.get(), now);
    }

    bool doTTLSubPassForTest(OperationContext* opCtx, Date_t now) {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());

        return ttlMonitor->_doTTLSubPass(opCtx, now);
    }

    long long getTTLPasses() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlPasses);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLPasses_forTest();
    }

    long long getTTLSubPasses() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlSubPasses);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLSubPasses_forTest();
    }

    long long getTTLDurationMicros() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlDuration);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLDurationMicros_forTest();
    }

    long long getTTLDeletedDocuments() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlDeletedDocuments);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLDeletedDocuments_forTest();
    }

    long long getTTLDeletedKeys() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlDeletedKeys);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLDeletedKeys_forTest();
    }

    long long getTTLExaminedDocuments() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlExaminedDocuments);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLExaminedDocuments_forTest();
    }

    long long getTTLExaminedKeys() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlExaminedKeys);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getTTLExaminedKeys_forTest();
    }

    long long getInvalidTTLIndexSkips() {
        if (_capturer.canReadMetrics()) {
            return _capturer.readInt64Counter(otel::metrics::MetricNames::kTtlInvalidTtlIndexSkips);
        }
        return TTLMonitor::get(getGlobalServiceContext())->getInvalidTTLIndexSkips_forTest();
    }

    // Asserts that the 'indexSpec' is persisted as-is in the durable catalog.
    void assertDurableIndexSpec(const NamespaceString& nss, const BSONObj& indexSpec) {
        ASSERT(indexSpec.hasField("name"));
        const auto indexName = indexSpec.getStringField("name");

        const auto durableCatalogEntry =
            durable_catalog::scanForCatalogEntryByNss(opCtx(), nss, MDBCatalog::get(opCtx()));
        ASSERT(durableCatalogEntry);
        const auto collMetaData = durableCatalogEntry->metadata;
        const auto idxOffset = collMetaData->findIndexOffset(indexName);
        ASSERT_GT(idxOffset, -1) << indexName;
        const auto indexMetaData = collMetaData->indexes[idxOffset];

        ASSERT_BSONOBJ_EQ(indexSpec, indexMetaData.spec);
    }

    // Helper to refetch the Collection from the catalog in order to see any changes made to it.
    CollectionPtr coll(OperationContext* opCtx, const NamespaceString& nss) {
        // TODO(SERVER-103400): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
        return CollectionPtr::CollectionPtr_UNSAFE(
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));
    }

    void createIndex(const NamespaceString& nss, const BSONObj& spec) {
        AutoGetCollection collection(opCtx(), nss, MODE_X);
        ASSERT(collection);
        auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx());
        auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
        auto fromMigrate = false;
        indexBuildsCoord->createIndex(
            opCtx(), collection->uuid(), spec, indexConstraints, fromMigrate);

        // Assert that 'spec' is stored exactly as-is in the durable catalog and the cached
        // 'indexCatalog'. In both internal code paths and in IDL generated commands, numerical
        // 'expireAfterSeconds' values are coerced to fit into an 'int32'. These assertions ensure
        // the designated index 'spec' is preserved for testing.
        //
        // This is particularly important when simulating 'expireAfterSeconds' values generated from
        // legacy versions of the server. In older versions, there weren't strict index field
        // checks, which made it possible for 'expireAfterSeconds' to be persisted in the catalog
        // with values incompatible with int32.
        assertDurableIndexSpec(nss, spec);
        const auto entry = coll(opCtx(), nss)
                               ->getIndexCatalog()
                               ->findIndexByName(opCtx(), spec.getStringField("name"));
        ASSERT_BSONOBJ_EQ(spec, entry->descriptor()->toBSON());
    }

    void createIndex(const NamespaceString& nss,
                     const BSONObj& keyPattern,
                     std::string name,
                     Seconds expireAfterSeconds) {
        const auto spec =
            BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << keyPattern << "name"
                     << name << "expireAfterSeconds" << durationCount<Seconds>(expireAfterSeconds));

        createIndex(nss, spec);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    otel::metrics::OtelMetricsCapturer _capturer;
};

namespace {

class SimpleClient {
public:
    SimpleClient(OperationContext* opCtx) : _client(opCtx), _opCtx(opCtx) {}

    void insert(const NamespaceString& nss, const BSONObj& obj) {
        _client.insert(nss, obj);
    }

    void insert(const NamespaceString& nss, const std::vector<BSONObj>& docs, bool ordered = true) {
        _client.insert(nss, docs, ordered);
    }

    long long count(const NamespaceString& nss) {
        return _client.count(nss);
    }

    // Inserts a set of expired documents expired on 'indexKey'. Additionally, each document has a
    // 'filter' field with 'indexKey' to aid in queries.
    void insertExpiredDocs(const NamespaceString& nss,
                           const std::string& indexKey,
                           int numExpiredDocs,
                           Seconds expireAfterSeconds = Seconds(1)) {
        Date_t now = Date_t::now();
        std::vector<BSONObj> expiredDocs{};
        for (auto i = 1; i <= numExpiredDocs; i++) {
            expiredDocs.emplace_back(BSON(indexKey << now - Seconds(i) - expireAfterSeconds));
        }
        insert(nss, expiredDocs);
    }

    void insertTimeseriesDocs(const NamespaceString& nss,
                              StringData timeField,
                              Date_t now,
                              Seconds interval,
                              int numDocs) {
        std::vector<BSONObj> docs{};
        for (auto i = 0; i < numDocs; i++) {
            docs.emplace_back(BSON(timeField << now + Seconds(interval * i)));
        }
        insert(nss, docs);
    }

    void createColl(const NamespaceString& nss) {
        ASSERT_OK(createCollection(_opCtx, nss, CollectionOptions{}, boost::none));
    }

    void createCollWithOptions(const NamespaceString& nss, const CollectionOptions& options) {
        ASSERT_OK(createCollection(_opCtx, nss, options, boost::none));
    }

    void setTimeseriesExtendedRange(const NamespaceString& nss) {
        auto resolvedNss = timeseries::test_util::resolveTimeseriesNss(nss);
        CollectionCatalog::get(_opCtx)
            ->lookupCollectionByNamespace(_opCtx, resolvedNss)
            ->setRequiresTimeseriesExtendedRangeSupport(_opCtx);
    }

private:
    DBDirectClient _client;
    OperationContext* _opCtx;
};

TEST_F(TTLTest, TTLPassSingleCollectionTwoIndexes) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    const auto docCount = 122;
    client.insertExpiredDocs(nss, "x", 120);
    client.insertExpiredDocs(nss, "y", 2);
    EXPECT_EQ(client.count(nss), docCount);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments + docCount);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys + docCount * 2);

    // The query planner can report more docs examined in case of write conflict retries or when
    // checking if document still matches after yielding.
    EXPECT_GE(getTTLExaminedDocuments(), initTTLExaminedDocuments + docCount);
    EXPECT_LE(getTTLExaminedDocuments(), initTTLExaminedDocuments + 2 * docCount);
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys + docCount);
}

TEST_F(TTLTest, TTLPassSingleCollectionSecondaryDoesNothing) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));

    client.insertExpiredDocs(nss, "x", 100);
    EXPECT_EQ(client.count(nss), 100);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx());
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));

    auto initTTLPasses = getTTLPasses();
    auto initTTLSubPasses = getTTLSubPasses();
    auto initTTLDurationMicros = getTTLDurationMicros();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // No documents are removed, no passes are incremented.
    EXPECT_EQ(client.count(nss), 100);
    EXPECT_EQ(getTTLPasses(), initTTLPasses);
    EXPECT_EQ(getTTLSubPasses(), initTTLSubPasses);
    EXPECT_EQ(getTTLDurationMicros(), initTTLDurationMicros);
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys);

    // The query planner only reports docs/keys examined to find documents to delete.
    EXPECT_EQ(getTTLExaminedDocuments(), initTTLExaminedDocuments);
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys);
}

TEST_F(TTLTest, TTLPassSingleCollectionClusteredIndexes) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    CollectionOptions options;
    options.clusteredIndex =
        ClusteredCollectionInfo(ClusteredIndexSpec(fromjson("{_id: 1}"), true), false);
    options.expireAfterSeconds = 1;
    client.createCollWithOptions(nss, options);

    const auto docCount = 100;
    client.insertExpiredDocs(nss, "_id", docCount);
    EXPECT_EQ(client.count(nss), docCount);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);

    // For a clustered collection without additional indexes, 0 keys deleted is valid.
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments + docCount);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys);

    // The query planner can report more docs examined in case of write conflict retries or when
    // checking if document still matches after yielding.
    EXPECT_GE(getTTLExaminedDocuments(), initTTLExaminedDocuments + docCount);
    EXPECT_LE(getTTLExaminedDocuments(), initTTLExaminedDocuments + 2 * docCount);
    // For a clustered collection without additional indexes, 0 keys examined is valid.
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys);
}

TEST_F(TTLTest, TTLPassSingleCollectionMixedIndexes) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    CollectionOptions options;
    options.clusteredIndex =
        ClusteredCollectionInfo(ClusteredIndexSpec(fromjson("{_id: 1}"), true), false);
    options.expireAfterSeconds = 1;
    client.createCollWithOptions(nss, options);
    createIndex(nss, BSON("foo" << 1), "fooIndex", Seconds(1));

    const auto docCount = 100;
    client.insertExpiredDocs(nss, "_id", 50);
    client.insertExpiredDocs(nss, "foo", 50);
    EXPECT_EQ(client.count(nss), docCount);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);

    // For a clustered collection with 1 additional index, 1 deleted key per document is valid.
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments + docCount);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys + docCount);

    // The query planner can report more docs examined in case of write conflict retries or when
    // checking if document still matches after yielding.
    EXPECT_GE(getTTLExaminedDocuments(), initTTLExaminedDocuments + docCount);
    EXPECT_LE(getTTLExaminedDocuments(), initTTLExaminedDocuments + 2 * docCount);
    // As the index on _id is clustered, only the keys examined on the foo index are counted.
    EXPECT_GE(getTTLExaminedKeys(), initTTLExaminedKeys + 50);
    EXPECT_LE(getTTLExaminedKeys(), initTTLExaminedKeys + 100);
}

TEST_F(TTLTest, TTLPassSingleCollectionMultipleDeletes) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);
    createIndex(nss, BSON("foo" << 1), "fooIndex", Seconds(1));

    const auto docCount = 50000;
    client.insertExpiredDocs(nss, "foo", 50000);
    EXPECT_EQ(client.count(nss), docCount);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments + docCount);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys + docCount);

    // The query planner can report more docs examined in case of write conflict retries or when
    // checking if document still matches after yielding.
    EXPECT_GE(getTTLExaminedDocuments(), initTTLExaminedDocuments + docCount);
    EXPECT_LE(getTTLExaminedDocuments(), initTTLExaminedDocuments + 2 * docCount);
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys + docCount);
}

TEST_F(TTLTest, TTLPassSingleTimeseriesSimpleDelete) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    std::string timeField = "t";
    int maxSpanSeconds = 20;
    int documents = maxSpanSeconds + 1;
    CollectionOptions options;
    options.expireAfterSeconds = 1;
    options.timeseries = TimeseriesOptions(timeField);
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);
    options.timeseries->setBucketRoundingSeconds(maxSpanSeconds);
    client.createCollWithOptions(nss, options);

    Date_t now = Date_t::now();
    // Insert documents starting at `2x maxSpanSeconds - 1` prior to when we will run the TTL
    // delete. Since there are `maxSpanSeconds + 1` documents, we insert the documents such that the
    // final document is safely below the `now-maxSpanSeconds` threshold, avoiding potential
    // timestamp comparison issues seen sporadically on Windows platforms. As no document is going
    // to go past `now-maxSpanSeconds`, all the inserted documents should be deleted by TTL.
    Date_t timeseriesStartTime = now - Seconds(maxSpanSeconds * 2) - Seconds(1);
    client.insertTimeseriesDocs(nss, timeField, timeseriesStartTime, Seconds(1), documents);
    EXPECT_EQ(client.count(nss), documents);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();
    auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(now);

    // Everything should be deleted.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);

    // For a (timeseries) clustered collection without additional indexes, 0 keys deleted is valid.
    // In this case, the number of documents deleted is the number of timeseries buckets (i.e. 2)
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments + 2);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys);

    // The query planner only reports docs/keys examined to find documents to delete.
    // For a clustered collection without additional indexes, 0 keys examined is valid.
    EXPECT_GT(getTTLExaminedDocuments(), initTTLExaminedDocuments);
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys);
}

TEST_F(TTLTest, TTLPassSingleTimeseriesSimpleUneligible) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    std::string timeField = "t";
    int maxSpanSeconds = 20;
    int documents = maxSpanSeconds * 2;
    CollectionOptions options;
    options.expireAfterSeconds = 1;
    options.timeseries = TimeseriesOptions(timeField);
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);
    options.timeseries->setBucketRoundingSeconds(maxSpanSeconds);
    client.createCollWithOptions(nss, options);

    Date_t now = Date_t::now();
    // Insert documents starting at now, no documents is then eligible for deletion.
    client.insertTimeseriesDocs(nss, timeField, now, Seconds(1), documents);
    EXPECT_EQ(client.count(nss), documents);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();

    doTTLPassForTest(now);

    // All documents remain after the TTL pass.
    EXPECT_EQ(client.count(nss), documents);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys);
}

TEST_F(TTLTest, TTLPassSingleTimeseriesBucketMaxSpan) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    std::string timeField = "t";
    int documents = 50;
    int maxSpanSeconds = 20;
    CollectionOptions options;
    options.expireAfterSeconds = 1;
    options.timeseries = TimeseriesOptions(timeField);
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);
    options.timeseries->setBucketRoundingSeconds(maxSpanSeconds);
    client.createCollWithOptions(nss, options);

    Date_t now = Date_t::now();
    // Data going back `maxSpanSeconds` from when the TTL delete runs might get deleted depending on
    // the rounding of `now`. TTL deletes on timeseries only delete buckets where the minTime +
    // maxSpanSeconds is less than the TTL deletion time. When we insert at now-maxSpanSeconds the
    // bucket minTime will be now-maxSpanSeconds-(now%roundingSeconds) resulting in now %
    // roundingSeconds documents being inserted into a bucket eligible for deletion.
    client.insertTimeseriesDocs(
        nss, timeField, now - Seconds(maxSpanSeconds), Seconds(1), documents);
    EXPECT_EQ(client.count(nss), documents);

    auto initTTLPasses = getTTLPasses();
    doTTLPassForTest(now);

    EXPECT_GE(client.count(nss), documents - maxSpanSeconds + options.expireAfterSeconds.value());
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
}

TEST_F(TTLTest, TTLPassTimeseriesExtendedPrior1970Delete) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    std::string timeField = "t";
    int maxSpanSeconds = 20;
    CollectionOptions options;
    options.expireAfterSeconds = 1;
    options.timeseries = TimeseriesOptions(timeField);
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);
    options.timeseries->setBucketRoundingSeconds(maxSpanSeconds);
    client.createCollWithOptions(nss, options);

    Date_t now = Date_t::now();
    // Insert one document prior to 1970 and then one document eligible for deletion and one not
    // eligible. The document prior to 1970 have such a time that when it is truncated to 4 bytes it
    // would not be deleted by the regular timeseries TTL deleter unless the collection is properly
    // marked as having extended range.
    client.insertTimeseriesDocs(nss, timeField, Date_t::fromMillisSinceEpoch(-1000), Seconds(1), 1);
    client.insertTimeseriesDocs(nss, timeField, now - Seconds(maxSpanSeconds * 2), Seconds(1), 1);
    client.insertTimeseriesDocs(nss, timeField, now, Seconds(1), 1);
    // Typically an opobserver marks the collection as extended range if needed. We don't have that
    // in this unit test so we set it here.
    client.setTimeseriesExtendedRange(nss);
    EXPECT_EQ(client.count(nss), 3);

    auto initTTLPasses = getTTLPasses();
    doTTLPassForTest(now);

    // We should delete two documents, the one prior to 1970 and the other eligible doc.
    EXPECT_EQ(client.count(nss), 1);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
}

TEST_F(TTLTest, TTLPassTimeseriesExtendedAfter2038Delete) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    std::string timeField = "t";
    int maxSpanSeconds = 20;
    CollectionOptions options;
    options.expireAfterSeconds = 1;
    options.timeseries = TimeseriesOptions(timeField);
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);
    options.timeseries->setBucketRoundingSeconds(maxSpanSeconds);
    client.createCollWithOptions(nss, options);

    Date_t now = Date_t::now();
    // Insert one document eligible for deletion and one far into the future marking the collection
    // with extended range. The document after 2038 is selected in such a way that it would be
    // deleted by the regular timeseries TTL deleter if the collection is not marked as extended
    // range.
    client.insertTimeseriesDocs(nss, timeField, now - Seconds(maxSpanSeconds * 2), Seconds(1), 1);
    client.insertTimeseriesDocs(nss, timeField, Date_t() + Seconds(0x10000FFFF), Seconds(1), 1);
    // Typically an opobserver marks the collection as extended range if needed. We don't have that
    // in this unit test so we set it here.
    client.setTimeseriesExtendedRange(nss);
    EXPECT_EQ(client.count(nss), 2);

    const auto initTTLPasses = getTTLPasses();
    doTTLPassForTest(now);

    // The document with time 1940 should remain.
    EXPECT_EQ(client.count(nss), 1);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
}

TEST_F(TTLTest, TTLPassCollectionWithoutExpiration) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);
    AutoGetCollection collection(opCtx(), nss, MODE_X);
    ASSERT(collection);
    auto spec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1) << "name"
                 << "fooIndex");
    createIndex(nss, spec);

    client.insertExpiredDocs(nss, "foo", 100);
    EXPECT_EQ(client.count(nss), 100);

    const auto initTTLPasses = getTTLPasses();
    const auto initInvalidTTLIndexSkips = getInvalidTTLIndexSkips();
    const auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    const auto initTTLDeletedKeys = getTTLDeletedKeys();
    const auto initTTLExaminedDocuments = getTTLExaminedDocuments();
    const auto initTTLExaminedKeys = getTTLExaminedKeys();

    doTTLPassForTest(Date_t::now());

    // No documents are removed.
    EXPECT_EQ(client.count(nss), 100);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(), initTTLDeletedDocuments);
    EXPECT_EQ(getTTLDeletedKeys(), initTTLDeletedKeys);

    // The query planner only reports docs/keys examined to find documents to delete.
    EXPECT_EQ(getTTLExaminedDocuments(), initTTLExaminedDocuments);
    EXPECT_EQ(getTTLExaminedKeys(), initTTLExaminedKeys);

    // A non-TTL index doesn't contribute to the number of skipped invalid TTL indexes.
    EXPECT_EQ(getInvalidTTLIndexSkips(), initInvalidTTLIndexSkips);
}


TEST_F(TTLTest, TTLPassMultipCollectionsPass) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("testDB.coll0");
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("testDB.coll1");

    client.createColl(nss0);
    client.createColl(nss1);

    createIndex(nss0, BSON("x" << 1), "testIndexX", Seconds(1));

    createIndex(nss1, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss1, BSON("y" << 1), "testIndexY", Seconds(1));

    int xExpiredDocsNss0 = 5;
    int xExpiredDocsNss1 = 530;
    int yExpiredDocsNss1 = 60;

    client.insertExpiredDocs(nss0, "x", xExpiredDocsNss0);
    client.insertExpiredDocs(nss1, "x", xExpiredDocsNss1);
    client.insertExpiredDocs(nss1, "y", yExpiredDocsNss1);

    EXPECT_EQ(client.count(nss0), xExpiredDocsNss0);
    EXPECT_EQ(client.count(nss1), xExpiredDocsNss1 + yExpiredDocsNss1);

    auto initTTLPasses = getTTLPasses();
    auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    auto initTTLDeletedKeys = getTTLDeletedKeys();

    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss0), 0);
    EXPECT_EQ(client.count(nss1), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(),
              xExpiredDocsNss0 + xExpiredDocsNss1 + yExpiredDocsNss1 + initTTLDeletedDocuments);
    EXPECT_EQ(getTTLDeletedKeys(),
              xExpiredDocsNss0 + ((xExpiredDocsNss1 + yExpiredDocsNss1) * 2) + initTTLDeletedKeys);
}

// Demonstrate sub-pass behavior when all expired documents are drained before the sub-pass reaches
// its time limit.
TEST_F(TTLTest, TTLSingleSubPass) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set 'ttlMonitorSubPasstargetSecs' to a day to guarantee the sub-pass target time is never
    // reached.
    unittest::ServerParameterGuard ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", 60 * 60 * 24);

    // Each batched delete issued on a TTL index will only delete up to ttlIndexDeleteTargetDocs.
    auto ttlIndexDeleteTargetDocs = 20;
    unittest::ServerParameterGuard ttlIndexDeleteTargetDocsController("ttlIndexDeleteTargetDocs",
                                                                      ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // Require multiple iterations of batched deletes over each index for the sub-pass.
    int xExpiredDocs = ttlIndexDeleteTargetDocs * 4;
    int yExpiredDocs = ttlIndexDeleteTargetDocs + 10;

    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs);

    auto currentCount = client.count(nss);
    EXPECT_EQ(currentCount, xExpiredDocs + yExpiredDocs);

    bool moreWork = doTTLSubPassForTest(opCtx(), Date_t::now());

    // A sub-pass removes all expired document provided it does not reach
    // 'ttlMonitorSubPassTargetSecs'.
    EXPECT_FALSE(moreWork);
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLSubPasses(), nInitialSubPasses + 1);
}

TEST_F(TTLTest, TTLSubPassesRemoveExpiredDocuments) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    unittest::ServerParameterGuard ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    unittest::ServerParameterGuard ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    unittest::ServerParameterGuard ttlIndexDeleteTargetDocsController("ttlIndexDeleteTargetDocs",
                                                                      ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // An exact multiple of (N * 'ttlIndexDeleteTargetDocs') documents expired on a TTL index
    // requires (N + 1) batched deletes on the TTL index. The first N batched deletes reach
    // 'ttlIndexTargetDocs' before exhausting all documents. For simplictly, compute the number of
    // expired documents as (N * 'ttlIndexDeleteTargetDocs' - 1) so N can be set to the expected
    // number of sub-passes executed in this test.
    int nExpectedSubPasses = 3;
    int xExpiredDocs = ttlIndexDeleteTargetDocs * nExpectedSubPasses - 1;
    int yExpiredDocs = 1;

    int nExpectedTotalSubPasses = nInitialSubPasses + nExpectedSubPasses;

    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs);

    auto currentCount = client.count(nss);
    EXPECT_EQ(currentCount, xExpiredDocs + yExpiredDocs);

    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx(), Date_t::now());
        EXPECT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs + ttlIndexDeleteTargetDocs;
        auto newCount = client.count(nss);
        EXPECT_EQ(newCount, currentCount - expectedDocsRemoved);
        currentCount = newCount;
    }

    while ((moreWork = doTTLSubPassForTest(opCtx(), Date_t::now())) == true) {
        auto newCount = client.count(nss);
        EXPECT_EQ(newCount, currentCount - ttlIndexDeleteTargetDocs);
        currentCount = newCount;
    }

    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLSubPasses(), nExpectedTotalSubPasses);
}

TEST_F(TTLTest, TTLSubPassesRemoveExpiredDocumentsAddedBetweenSubPasses) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    unittest::ServerParameterGuard ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    unittest::ServerParameterGuard ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    unittest::ServerParameterGuard ttlIndexDeleteTargetDocsController("ttlIndexDeleteTargetDocs",
                                                                      ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // Intentionally set xExpiredDocs to require more than one sub-pass.
    int xExpiredDocs = ttlIndexDeleteTargetDocs * 2;
    int yExpiredDocs0 = 1;

    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs0);

    auto initialNDocuments = client.count(nss);
    EXPECT_EQ(initialNDocuments, xExpiredDocs + yExpiredDocs0);

    auto nSubPasses = getTTLSubPasses();
    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx(), Date_t::now());
        EXPECT_EQ(getTTLSubPasses(), ++nSubPasses);

        EXPECT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs0, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs0 + ttlIndexDeleteTargetDocs;

        EXPECT_EQ(client.count(nss), initialNDocuments - expectedDocsRemoved);
    }

    // While the TTL index on 'y' is exhausted (all expired documents have been removed in the first
    // sub-pass), there is still more work to do on TTL index 'x'. Demonstrate that additional
    // expired documents on a previously exhausted TTL index are cleaned up between sub-passes.
    auto expectedAdditionalSubPasses = 3;
    auto expectedTotalSubPasses = nSubPasses + expectedAdditionalSubPasses;

    // An exact multiple of 'ttlIndexDeleteTargetDocs' on TTL index 'y' means an additional
    // subpass is necessary to know there is no more work after the target is met. Subtract 1
    // document for simplicitly.
    auto yExpiredDocs1 = ttlIndexDeleteTargetDocs * expectedAdditionalSubPasses - 1;

    auto nDocumentsBeforeInsert = client.count(nss);
    client.insertExpiredDocs(nss, "y", yExpiredDocs1);
    EXPECT_EQ(client.count(nss), nDocumentsBeforeInsert + yExpiredDocs1);

    while (doTTLSubPassForTest(opCtx(), Date_t::now())) {
    }

    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLSubPasses(), expectedTotalSubPasses);
}

// Tests that, between sub-passes, newly added TTL indexes are not ignored.
TEST_F(TTLTest, TTLSubPassesStartRemovingFromNewTTLIndex) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    unittest::ServerParameterGuard ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    unittest::ServerParameterGuard ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    unittest::ServerParameterGuard ttlIndexDeleteTargetDocsController("ttlIndexDeleteTargetDocs",
                                                                      ttlIndexDeleteTargetDocs);


    SimpleClient client(opCtx());
    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // Intentionally set xExpiredDocs to require more than one sub-pass.
    int xExpiredDocs = ttlIndexDeleteTargetDocs * 2;
    int yExpiredDocs = 1;
    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs);

    // Insert zDocs that are not expired by an existing TTL index.
    int zDocs = ttlIndexDeleteTargetDocs * 4 - 1;
    client.insertExpiredDocs(nss, "z", zDocs);

    auto currentCount = client.count(nss);
    EXPECT_EQ(currentCount, xExpiredDocs + yExpiredDocs + zDocs);

    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx(), Date_t::now());
        EXPECT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs + ttlIndexDeleteTargetDocs;
        auto newCount = client.count(nss);

        EXPECT_EQ(newCount, currentCount - expectedDocsRemoved);

        currentCount = newCount;
    }

    // Each sub-pass refreshes its view of the current TTL indexes. Before the next sub-pass, create
    // a new TTL index.
    createIndex(nss, BSON("z" << 1), "testIndexZ", Seconds(1));

    do {
        moreWork = doTTLSubPassForTest(opCtx(), Date_t::now());
    } while (moreWork);

    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLSubPasses(), 5 + nInitialSubPasses);
}

// Simple test using the ttlmonitor's internal thread to exercise the scheduling logic.
// This involves manual sleeps; we will just test this way once and test the pass
// function directly in all other tests of ttl logic.
TEST_F(TTLTest, TTLRunMonitorThread) {
    unittest::ServerParameterGuard ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));

    client.insertExpiredDocs(nss, "x", 100);
    EXPECT_EQ(client.count(nss), 100);

    // Let the monitor run a pass.
    auto initTTLPasses = getTTLPasses();
    auto initTTLDurationMicros = getTTLDurationMicros();
    TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
    ttlMonitor->go();
    ASSERT_OK(ttlMonitor->onUpdateTTLMonitorSleepSeconds(0));
    std::this_thread::sleep_for(Milliseconds(1000).toSystemDuration());

    // Shut down the monitor, we need to wait for the _shuttingDown
    // flag to be processed.
    shutdownTTLMonitor(getGlobalServiceContext());
    ASSERT_OK(ttlMonitor->onUpdateTTLMonitorSleepSeconds(0));
    std::this_thread::sleep_for(Milliseconds(1000).toSystemDuration());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_GT(getTTLPasses(), initTTLPasses);  // More than one may have been run
    EXPECT_GT(getTTLDurationMicros(), initTTLDurationMicros);
}

// Values smaller than int32_t::max() are valid for secondary TTL indexes.
TEST_F(TTLTest, TTLDoubleFitsIntoInt32) {
    SimpleClient client(opCtx());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);
    const double expireAfterSeconds = 4.5;
    const auto validSpec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1) << "name"
                 << "smallEnough"
                 << "expireAfterSeconds" << expireAfterSeconds);
    createIndex(nss, validSpec);
    const auto nDocs = 10;
    Seconds expireAfterSecondsRounded(5);
    client.insertExpiredDocs(nss, "foo", nDocs, expireAfterSecondsRounded);
    EXPECT_EQ(client.count(nss), nDocs);

    const auto initTTLPasses = getTTLPasses();
    const auto initInvalidTTLIndexSkips = getInvalidTTLIndexSkips();
    const auto initTTLDeletedDocuments = getTTLDeletedDocuments();
    const auto initTTLDeletedKeys = getTTLDeletedKeys();
    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getTTLDeletedDocuments(), nDocs + initTTLDeletedDocuments);
    EXPECT_EQ(getTTLDeletedKeys(), nDocs + initTTLDeletedKeys);
    EXPECT_EQ(getInvalidTTLIndexSkips(), initInvalidTTLIndexSkips);
}

TEST_F(TTLTest, TTLMinDoubleFitsIntoInt32) {
    SimpleClient client(opCtx());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");

    client.createColl(nss);
    const double expireAfterSeconds = std::numeric_limits<double>::min();
    const auto validSpec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1) << "name"
                 << "minDoubleExpiry"
                 << "expireAfterSeconds" << expireAfterSeconds);
    createIndex(nss, validSpec);
    const auto nDocs = 10;
    client.insertExpiredDocs(nss, "foo", nDocs);
    EXPECT_EQ(client.count(nss), nDocs);

    const auto initTTLPasses = getTTLPasses();
    const auto initInvalidTTLIndexSkips = getInvalidTTLIndexSkips();
    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getInvalidTTLIndexSkips(), initInvalidTTLIndexSkips);
}

TEST_F(TTLTest, TTLkExpireAfterSecondsForInactiveTTLIndexIsValid) {
    SimpleClient client(opCtx());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.coll0");
    client.createColl(nss);

    // Historically, NaN values were automatically adjusted to
    // 'kExpireAfterSecondsForInactiveTTLIndex'. Any entries with this expiry won't be removed for a
    // very long time, but expiry set to the maximum int32 value is still valid for a TTL index.
    const auto expireAfterSeconds = index_key_validate::kExpireAfterSecondsForInactiveTTLIndex;
    const auto validSpec =
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1) << "name"
                 << "TTLExpiryForInactiveTTL"
                 << "expireAfterSeconds" << durationCount<Seconds>(expireAfterSeconds));
    createIndex(nss, validSpec);
    const auto nDocs = 10;
    client.insertExpiredDocs(nss, "foo", nDocs, expireAfterSeconds);
    EXPECT_EQ(client.count(nss), nDocs);

    const auto initTTLPasses = getTTLPasses();
    const auto initInvalidTTLIndexSkips = getInvalidTTLIndexSkips();
    doTTLPassForTest(Date_t::now());

    // All expired documents are removed.
    EXPECT_EQ(client.count(nss), 0);
    EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);
    EXPECT_EQ(getInvalidTTLIndexSkips(), initInvalidTTLIndexSkips);
}

// Tests invalid TTL indexes are skipped for document deletion. Theoretically, there should never be
// invalid TTL indexes. However, this is a safety check given older versions of the server permitted
// TTL fields that don't fit into an int32.
class SkipInvalidTTLTest : public TTLTest {
protected:
    void runTestCase(BSONObj&& spec) {
        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("testDB.invalidTTLColl");

        SimpleClient client(opCtx());
        client.createColl(nss);
        createIndex(nss, spec);
        auto catalogEntry =
            durable_catalog::scanForCatalogEntryByNss(opCtx(), nss, MDBCatalog::get(opCtx()));
        ASSERT(catalogEntry);

        const int nDocs = 10;
        client.insertExpiredDocs(nss, "foo", nDocs);
        EXPECT_EQ(client.count(nss), nDocs);

        const auto initTTLPasses = getTTLPasses();
        const auto initInvalidTTLIndexSkips = getInvalidTTLIndexSkips();

        doTTLPassForTest(Date_t::now());

        // No documents are removed.
        EXPECT_EQ(client.count(nss), nDocs);
        EXPECT_EQ(getTTLPasses(), initTTLPasses + 1);

        // Metrics track an invalid TTL index was skipped during the pass.
        EXPECT_EQ(getInvalidTTLIndexSkips(), initInvalidTTLIndexSkips + 1);
    }
};

TEST_F(SkipInvalidTTLTest, TTLMaxLongExpireAfterSeconds) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "maxLongExpiry"
                         << "expireAfterSeconds" << LLONG_MAX));
}

TEST_F(SkipInvalidTTLTest, TTLMinLongExpireAfterSeconds) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "minLongExpiry"
                         << "expireAfterSeconds" << LLONG_MIN));
}

TEST_F(SkipInvalidTTLTest, TTLBasicNegativeExpireAfterSeconds) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "basicNegExpiry"
                         << "expireAfterSeconds" << -1));
}

TEST_F(SkipInvalidTTLTest, TTLMinInt32ExpireAfterSeconds) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "minIntExpiry"
                         << "expireAfterSeconds" << std::numeric_limits<int32_t>::min()));
}

TEST_F(SkipInvalidTTLTest, TTLNonNumericExpireAfterSeconds) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "nonNumericExpiry"
                         << "expireAfterSeconds"
                         << "stringValue"));
}

TEST_F(SkipInvalidTTLTest, TTLSpecialIndexType) {
    // An index not of type IndexType::INDEX_BTREE isn't permitted for TTL expiration.
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key"
                         << BSON("foo" << "hashed") << "name"
                         << "hashedIndex"
                         << "expireAfterSeconds" << 10));
}

TEST_F(SkipInvalidTTLTest, TTLCompoundIndex) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key"
                         << BSON("foo" << 1 << "secondIdxKey" << 1) << "name"
                         << "compoundIndex"
                         << "expireAfterSeconds" << 10));
}

TEST_F(SkipInvalidTTLTest, TTLPosNan) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "posNanExpiry"
                         << "expireAfterSeconds" << std::numeric_limits<double>::quiet_NaN()));
}

TEST_F(SkipInvalidTTLTest, TTLNegNan) {
    runTestCase(BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << BSON("foo" << 1)
                         << "name"
                         << "negNanExpiry"
                         << "expireAfterSeconds" << -std::numeric_limits<double>::quiet_NaN()));
}

}  // namespace
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
