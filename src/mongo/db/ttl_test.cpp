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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/ttl.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

// Must exist in the mongo namespace to be a friend class of the TTLMonitor.
class TTLTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        std::unique_ptr<TTLMonitor> ttlMonitor = std::make_unique<TTLMonitor>();
        TTLMonitor::set(service, std::move(ttlMonitor));

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

    void doTTLPassForTest() {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
        ttlMonitor->_doTTLPass();
    }

    bool doTTLSubPassForTest(OperationContext* opCtx) {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
        return ttlMonitor->_doTTLSubPass(opCtx);
    }

    long long getTTLPasses() {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
        return ttlMonitor->getTTLPasses_forTest();
    }

    long long getTTLSubPasses() {
        TTLMonitor* ttlMonitor = TTLMonitor::get(getGlobalServiceContext());
        return ttlMonitor->getTTLSubPasses_forTest();
    }

    // Bypasses the need for a two-phase index build with a commit quorum through DBClient.
    void createIndex(const NamespaceString& nss,
                     const BSONObj& keyPattern,
                     std::string name,
                     Seconds expireAfterSeconds) {

        AutoGetCollection collection(opCtx(), nss, MODE_X);
        ASSERT(collection);

        auto spec =
            BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << keyPattern << "name"
                     << name << "expireAfterSeconds" << durationCount<Seconds>(expireAfterSeconds));

        auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx());

        auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
        auto fromMigrate = false;
        indexBuildsCoord->createIndex(
            opCtx(), collection->uuid(), spec, indexConstraints, fromMigrate);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

namespace {

class SimpleClient {
public:
    SimpleClient(OperationContext* opCtx) : _client(opCtx), _opCtx(opCtx) {}

    void insert(const NamespaceString& nss, const BSONObj& obj) {
        _client.insert(nss.ns(), obj);
    }

    void insert(const NamespaceString& ns, const std::vector<BSONObj>& docs, bool ordered = true) {
        _client.insert(ns.ns(), docs, ordered);
    }

    long long count(const NamespaceString& nss) {
        return _client.count(nss);
    }

    // Inserts a set of expired documents expired on 'indexKey'. Additionally, each document has a
    // 'filter' field with 'indexKey' to aid in queries.
    void insertExpiredDocs(const NamespaceString& nss,
                           const std::string& indexKey,
                           int numExpiredDocs) {
        Date_t now = Date_t::now();
        std::vector<BSONObj> expiredDocs{};
        for (auto i = 1; i <= numExpiredDocs; i++) {
            expiredDocs.emplace_back(BSON(indexKey << now - Seconds(i)));
        }
        insert(nss, expiredDocs);
    }

    void createCollection(const NamespaceString& nss) {
        _client.createCollection(nss.toString());
    }

private:
    DBDirectClient _client;
    OperationContext* _opCtx;
};

TEST_F(TTLTest, TTLPassSingleCollectionTwoIndexes) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss("testDB.coll0");

    client.createCollection(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    client.insertExpiredDocs(nss, "x", 120);
    client.insertExpiredDocs(nss, "y", 2);
    ASSERT_EQ(client.count(nss), 122);

    auto initTTLPasses = getTTLPasses();
    stdx::thread thread([&]() {
        // TTLMonitor::doTTLPass creates a new OperationContext, which cannot be done on the current
        // client because the OperationContext already exists.
        ThreadClient threadClient(getGlobalServiceContext());
        doTTLPassForTest();
    });
    thread.join();

    // All expired documents are removed.
    ASSERT_EQ(client.count(nss), 0);
    ASSERT_EQ(getTTLPasses(), initTTLPasses + 1);
}

TEST_F(TTLTest, TTLPassMultipCollectionsPass) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    SimpleClient client(opCtx());

    NamespaceString nss0("testDB.coll0");
    NamespaceString nss1("testDB.coll1");

    client.createCollection(nss0);
    client.createCollection(nss1);

    createIndex(nss0, BSON("x" << 1), "testIndexX", Seconds(1));

    createIndex(nss1, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss1, BSON("y" << 1), "testIndexY", Seconds(1));

    int xExpiredDocsNss0 = 5;
    int xExpiredDocsNss1 = 530;
    int yExpiredDocsNss1 = 60;

    client.insertExpiredDocs(nss0, "x", xExpiredDocsNss0);
    client.insertExpiredDocs(nss1, "x", xExpiredDocsNss1);
    client.insertExpiredDocs(nss1, "y", yExpiredDocsNss1);

    ASSERT_EQ(client.count(nss0), xExpiredDocsNss0);
    ASSERT_EQ(client.count(nss1), xExpiredDocsNss1 + yExpiredDocsNss1);

    auto initTTLPasses = getTTLPasses();

    stdx::thread thread([&]() {
        // TTLMonitor::doTTLPass creates a new OperationContext, which cannot be done on the
        // current client because the OperationContext already exists.
        ThreadClient threadClient(getGlobalServiceContext());
        doTTLPassForTest();
    });
    thread.join();

    // All expired documents are removed.
    ASSERT_EQ(client.count(nss0), 0);
    ASSERT_EQ(client.count(nss1), 0);

    ASSERT_EQ(getTTLPasses(), initTTLPasses + 1);
}

// Demonstrate sub-pass behavior when all expired documents are drained before the sub-pass reaches
// its time limit.
TEST_F(TTLTest, TTLSingleSubPass) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set 'ttlMonitorSubPasstargetSecs' to a day to guarantee the sub-pass target time is never
    // reached.
    RAIIServerParameterControllerForTest ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", 60 * 60 * 24);

    // Each batched delete issued on a TTL index will only delete up to ttlIndexDeleteTargetDocs.
    auto ttlIndexDeleteTargetDocs = 20;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetDocsController(
        "ttlIndexDeleteTargetDocs", ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss("testDB.coll");

    client.createCollection(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // Require multiple iterations of batched deletes over each index for the sub-pass.
    int xExpiredDocs = ttlIndexDeleteTargetDocs * 4;
    int yExpiredDocs = ttlIndexDeleteTargetDocs + 10;

    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs);

    auto currentCount = client.count(nss);
    ASSERT_EQ(currentCount, xExpiredDocs + yExpiredDocs);

    bool moreWork = doTTLSubPassForTest(opCtx());

    // A sub-pass removes all expired document provided it does not reach
    // 'ttlMonitorSubPassTargetSecs'.
    ASSERT_FALSE(moreWork);
    ASSERT_EQ(client.count(nss), 0);
    ASSERT_EQ(getTTLSubPasses(), nInitialSubPasses + 1);
}

TEST_F(TTLTest, TTLSubPassesRemoveExpiredDocuments) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    RAIIServerParameterControllerForTest ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetDocsController(
        "ttlIndexDeleteTargetDocs", ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss("testDB.coll");

    client.createCollection(nss);

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
    ASSERT_EQ(currentCount, xExpiredDocs + yExpiredDocs);

    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx());
        ASSERT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs + ttlIndexDeleteTargetDocs;
        auto newCount = client.count(nss);
        ASSERT_EQ(newCount, currentCount - expectedDocsRemoved);
        currentCount = newCount;
    }

    while ((moreWork = doTTLSubPassForTest(opCtx())) == true) {
        auto newCount = client.count(nss);
        ASSERT_EQ(newCount, currentCount - ttlIndexDeleteTargetDocs);
        currentCount = newCount;
    }

    ASSERT_EQ(client.count(nss), 0);
    ASSERT_EQ(getTTLSubPasses(), nExpectedTotalSubPasses);
}

TEST_F(TTLTest, TTLSubPassesRemoveExpiredDocumentsAddedBetweenSubPasses) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    RAIIServerParameterControllerForTest ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetDocsController(
        "ttlIndexDeleteTargetDocs", ttlIndexDeleteTargetDocs);

    SimpleClient client(opCtx());

    NamespaceString nss("testDB.coll");

    client.createCollection(nss);

    createIndex(nss, BSON("x" << 1), "testIndexX", Seconds(1));
    createIndex(nss, BSON("y" << 1), "testIndexY", Seconds(1));

    // Intentionally set xExpiredDocs to require more than one sub-pass.
    int xExpiredDocs = ttlIndexDeleteTargetDocs * 2;
    int yExpiredDocs0 = 1;

    client.insertExpiredDocs(nss, "x", xExpiredDocs);
    client.insertExpiredDocs(nss, "y", yExpiredDocs0);

    auto initialNDocuments = client.count(nss);
    ASSERT_EQ(initialNDocuments, xExpiredDocs + yExpiredDocs0);

    auto nSubPasses = getTTLSubPasses();
    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx());
        ASSERT_EQ(getTTLSubPasses(), ++nSubPasses);

        ASSERT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs0, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs0 + ttlIndexDeleteTargetDocs;

        ASSERT_EQ(client.count(nss), initialNDocuments - expectedDocsRemoved);
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
    ASSERT_EQ(client.count(nss), nDocumentsBeforeInsert + yExpiredDocs1);

    while (doTTLSubPassForTest(opCtx())) {
    }

    ASSERT_EQ(client.count(nss), 0);
    ASSERT_EQ(getTTLSubPasses(), expectedTotalSubPasses);
}

// Tests that, between sub-passes, newly added TTL indexes are not ignored.
TEST_F(TTLTest, TTLSubPassesStartRemovingFromNewTTLIndex) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBatchMultiDeletes",
                                                               true);
    RAIIServerParameterControllerForTest ttlBatchDeletesController("ttlMonitorBatchDeletes", true);

    // Set the target time for each sub-pass to 0 to test when only a single iteration of deletes is
    // performed on TTL indexes per sub pass.
    //
    // Enables testing of 'fairness' (without having explicit control over how long deletes take) -
    // that a limited amount of documents are removed from each TTL index before moving to the next
    // TTL index, regardless of the number of expired documents remaining.
    auto ttlMonitorSubPassTargetSecs = 0;
    RAIIServerParameterControllerForTest ttlMonitorSubPassTargetSecsController(
        "ttlMonitorSubPassTargetSecs", ttlMonitorSubPassTargetSecs);

    // Do not limit the amount of time in performing a batched delete each pass by setting
    // the target time to 0.
    auto ttlIndexDeleteTargetTimeMS = 0;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetTimeMSController(
        "ttlIndexDeleteTargetTimeMS", ttlIndexDeleteTargetTimeMS);

    // Expect each sub-pass to delete up to 20 documents from each index.
    auto ttlIndexDeleteTargetDocs = 20;
    RAIIServerParameterControllerForTest ttlIndexDeleteTargetDocsController(
        "ttlIndexDeleteTargetDocs", ttlIndexDeleteTargetDocs);


    SimpleClient client(opCtx());
    // The number of sub-passes is cumulative over the course of the TTLTest suite. The total
    // expected sub-passes differs from the expected sub-passes in the indidual test.
    int nInitialSubPasses = getTTLSubPasses();

    NamespaceString nss("testDB.coll");

    client.createCollection(nss);

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
    ASSERT_EQ(currentCount, xExpiredDocs + yExpiredDocs + zDocs);

    bool moreWork = true;

    // Issue first subpass.
    {
        moreWork = doTTLSubPassForTest(opCtx());
        ASSERT_TRUE(moreWork);

        // Since there were less than ttlIndexDeleteTargetDocs yExpiredDocs, expect all of the
        // yExpired docs removed.
        auto expectedDocsRemoved = yExpiredDocs + ttlIndexDeleteTargetDocs;
        auto newCount = client.count(nss);

        ASSERT_EQ(newCount, currentCount - expectedDocsRemoved);

        currentCount = newCount;
    }

    // Each sub-pass refreshes its view of the current TTL indexes. Before the next sub-pass, create
    // a new TTL index.
    createIndex(nss, BSON("z" << 1), "testIndexZ", Seconds(1));

    do {
        moreWork = doTTLSubPassForTest(opCtx());
    } while (moreWork);

    ASSERT_EQ(client.count(nss), 0);
    ASSERT_EQ(getTTLSubPasses(), 5 + nInitialSubPasses);
}

}  // namespace
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
