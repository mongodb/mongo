// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_local.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class ShardLocalTest : public ServiceContextMongoDTest {
protected:
    ShardLocalTest() : ServiceContextMongoDTest(Options{}.useReplSettings(true)) {
        serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    }

    ~ShardLocalTest() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = getGlobalServiceContext()->makeOperationContext(&cc());
        auto& shardSharedStateCache = ShardSharedStateCache::get(_opCtx.get());
        _shardLocal = std::make_unique<ShardLocal>(
            ShardHandle(ShardId::kConfigServerId, UUID::gen()),
            shardSharedStateCache.getShardState(ShardId::kConfigServerId));
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("mySet/node1:12345");
        repl::ReplicationCoordinator::set(
            getGlobalServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(
                new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings)));
        ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_PRIMARY));

        // Register an OpObserver so that writes reserve oplog slots and get commit timestamps.
        auto opObserverRegistry =
            checked_cast<OpObserverRegistry*>(getGlobalServiceContext()->getOpObserver());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        repl::createOplog(_opCtx.get());

        // Set a committed snapshot so that we can perform majority reads.
        WriteUnitOfWork wuow{_opCtx.get()};
        _opCtx->getServiceContext()->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            repl::getNextOpTime(_opCtx.get()).getTimestamp());
        wuow.commit();
    }

    void tearDown() override {
        _opCtx.reset();
        ServiceContextMongoDTest::tearDown();
        repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
    }

    /**
     * Sets up and runs a FindAndModify command with ShardLocal's runCommand. Finds a document in
     * namespace "nss" that matches "find" and updates the document with "set". Upsert and new are
     * set to true in the FindAndModify request.
     */
    StatusWith<Shard::CommandResponse> runFindAndModifyRunCommand(NamespaceString nss,
                                                                  BSONObj find,
                                                                  BSONObj set) {
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest(nss);
        findAndModifyRequest.setQuery(find);
        findAndModifyRequest.setUpdate(write_ops::UpdateModification::parseFromClassicUpdate(set));
        findAndModifyRequest.setUpsert(true);
        findAndModifyRequest.setNew(true);
        findAndModifyRequest.setWriteConcern(WriteConcernOptions(
            WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(15)));

        return _shardLocal->runCommand(_opCtx.get(),
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       nss.dbName(),
                                       findAndModifyRequest.toBSON(),
                                       Shard::RetryPolicy::kNoRetry);
    }

    /**
     * Advances both the storage engine's committed snapshot and the replication coordinator's
     * committed snapshot optime to the all durable timestamp, making all committed writes
     * majority-visible. Returns the new committed snapshot optime.
     */
    repl::OpTime advanceCommittedSnapshot() {
        const auto allDurable =
            _opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();
        _opCtx->getServiceContext()->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            allDurable);
        const repl::OpTime opTime{allDurable, getReplCoordMock()->getTerm()};
        getReplCoordMock()->setCurrentCommittedSnapshotOpTime(opTime);
        return opTime;
    }
    /**
     * Facilitates running a find query by supplying the redundant parameters. Finds documents in
     * namespace "nss" that match "query" and returns "limit" (if there are that many) number of
     * documents in "sort" order.
     */
    StatusWith<Shard::QueryResponse> runFindQuery(
        NamespaceString nss,
        BSONObj query,
        BSONObj sort,
        boost::optional<long long> limit,
        const repl::ReadConcernArgs& readConcern = repl::ReadConcernArgs::kMajority);

    repl::ReplicationCoordinatorMock* getReplCoordMock() {
        return checked_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(getGlobalServiceContext()));
    }

    /**
     * Returns the index definitions that exist for the given collection.
     */
    StatusWith<std::vector<BSONObj>> getIndexes(NamespaceString nss) {
        auto response = _shardLocal->runCommand(_opCtx.get(),
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                nss.dbName(),
                                                BSON("listIndexes" << nss.coll()),
                                                Shard::RetryPolicy::kIdempotent);
        if (!response.isOK()) {
            return response.getStatus();
        }
        if (!response.getValue().commandStatus.isOK()) {
            return response.getValue().commandStatus;
        }

        auto cursorResponse = CursorResponse::parseFromBSON(response.getValue().response);
        if (!cursorResponse.isOK()) {
            return cursorResponse.getStatus();
        }

        return cursorResponse.getValue().getBatch();
    }

    service_context_test::ShardRoleOverride _shardRole;

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ShardLocal> _shardLocal;
};

/**
 * Takes a FindAndModify command's BSON response and parses it for the returned "value" field.
 */
BSONObj extractFindAndModifyNewObj(const BSONObj& responseObj) {
    const auto& newDocElem = responseObj["value"];
    ASSERT(!newDocElem.eoo());
    ASSERT(newDocElem.isABSONObj());
    return newDocElem.Obj();
}

StatusWith<Shard::QueryResponse> ShardLocalTest::runFindQuery(
    NamespaceString nss,
    BSONObj query,
    BSONObj sort,
    boost::optional<long long> limit,
    const repl::ReadConcernArgs& readConcern) {
    return _shardLocal->exhaustiveFindOnConfig(_opCtx.get(),
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               readConcern,
                                               nss,
                                               query,
                                               sort,
                                               limit);
}

TEST_F(ShardLocalTest, RunCommand) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));

    Shard::CommandResponse commandResponse = unittest::assertGet(findAndModifyResponse);
    BSONObj newDocument = extractFindAndModifyNewObj(commandResponse.response);

    ASSERT_EQUALS(1, newDocument["fooItem"].numberInt());
    ASSERT_EQUALS(254, newDocument["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindOneWithoutLimit) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    // Set up documents to be queried.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 3), BSON("$set" << BSON("fooRandom" << 452)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Find a single document.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSON("fooItem" << 3), BSONObj(), boost::none);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 1;
    ASSERT_EQUALS(size, docs.size());
    BSONObj foundDoc = docs[0];
    ASSERT_EQUALS(3, foundDoc["fooItem"].numberInt());
    ASSERT_EQUALS(452, foundDoc["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindManyWithLimit) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    // Set up documents to be queried.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 2), BSON("$set" << BSON("fooRandom" << 444)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 3), BSON("$set" << BSON("fooRandom" << 452)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Find 2 of 3 documents.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSONObj(), BSON("fooItem" << 1), 2LL);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 2;
    ASSERT_EQUALS(size, docs.size());
    BSONObj firstDoc = docs[0];
    ASSERT_EQUALS(1, firstDoc["fooItem"].numberInt());
    ASSERT_EQUALS(254, firstDoc["fooRandom"].numberInt());
    BSONObj secondDoc = docs[1];
    ASSERT_EQUALS(2, secondDoc["fooItem"].numberInt());
    ASSERT_EQUALS(444, secondDoc["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindNoMatchingDocumentsEmpty) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    // Set up a document.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Run a query that won't find any results.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSON("fooItem" << 3), BSONObj(), boost::none);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 0;
    ASSERT_EQUALS(size, docs.size());
}

TEST_F(ShardLocalTest, MajorityReadConcernReadsAtStorageCommittedSnapshot) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)))
                  .getStatus());

    // Perform a write that is not yet reflected in the committed snapshot.
    getReplCoordMock()->setUpdateCommittedSnapshot(false);
    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 2), BSON("$set" << BSON("fooRandom" << 444)))
                  .getStatus());

    auto queryResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kMajority));
    ASSERT_EQUALS(1U, queryResponse.docs.size());
    ASSERT_EQUALS(1, queryResponse.docs[0]["fooItem"].numberInt());

    // Once the committed snapshot advances past both writes, majority sees them.
    advanceCommittedSnapshot();

    queryResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kMajority));
    ASSERT_EQUALS(2U, queryResponse.docs.size());
}

TEST_F(ShardLocalTest, SnapshotReadConcernReadsAtReplCoordCommittedSnapshotOpTime) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)))
                  .getStatus());
    const auto opTimeAfterFirstWrite = getReplCoordMock()->getCurrentCommittedSnapshotOpTime();
    ASSERT_FALSE(opTimeAfterFirstWrite.isNull());

    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 2), BSON("$set" << BSON("fooRandom" << 444)))
                  .getStatus());

    // Move only the replication coordinator's committed snapshot optime back to the first write;
    // the storage engine's committed snapshot still covers both writes.
    getReplCoordMock()->setCurrentCommittedSnapshotOpTime(opTimeAfterFirstWrite);

    // Snapshot reads at the replication coordinator's committed snapshot optime.
    auto snapshotResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kSnapshot));
    ASSERT_EQUALS(1U, snapshotResponse.docs.size());
    ASSERT_EQUALS(1, snapshotResponse.docs[0]["fooItem"].numberInt());

    // Majority reads at the storage engine's committed snapshot, so it sees both writes.
    auto majorityResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kMajority));
    ASSERT_EQUALS(2U, majorityResponse.docs.size());
}

TEST_F(ShardLocalTest, SnapshotWithAtClusterTimeReadsAtProvidedTimestamp) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

    // First write, represents the "past" state.
    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)))
                  .getStatus());
    const auto opTimeAfterFirstWrite = getReplCoordMock()->getCurrentCommittedSnapshotOpTime();
    ASSERT_FALSE(opTimeAfterFirstWrite.isNull());

    // Second write, represents the "majority" state.
    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 2), BSON("$set" << BSON("fooRandom" << 612)))
                  .getStatus());
    const auto opTimeAfterSecondWrite = getReplCoordMock()->getCurrentCommittedSnapshotOpTime();
    ASSERT_FALSE(opTimeAfterSecondWrite.isNull());

    // The third write is not reflected in either the storage engine's committed snapshot or the
    // replication coordinator's committed snapshot optime, which both remain at the second write.
    getReplCoordMock()->setUpdateCommittedSnapshot(false);
    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 3), BSON("$set" << BSON("fooRandom" << 444)))
                  .getStatus());
    const auto timestampAfterThirdWrite =
        _opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();
    ASSERT_GT(timestampAfterThirdWrite, opTimeAfterSecondWrite.getTimestamp());

    // Plain majority and snapshot reads only see the first write.
    auto majorityResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kMajority));
    ASSERT_EQUALS(2U, majorityResponse.docs.size());
    auto snapshotResponse = unittest::assertGet(runFindQuery(
        nss, BSONObj(), BSON("fooItem" << 1), boost::none, repl::ReadConcernArgs::kSnapshot));
    ASSERT_EQUALS(2U, snapshotResponse.docs.size());

    // atClusterTime overrides the committed snapshot and reads at the provided timestamp.
    auto atSecondWriteResponse = unittest::assertGet(
        runFindQuery(nss,
                     BSONObj(),
                     BSON("fooItem" << 1),
                     boost::none,
                     repl::ReadConcernArgs::snapshot(timestampAfterThirdWrite)));
    ASSERT_EQUALS(3U, atSecondWriteResponse.docs.size());

    // A past atClusterTime reads in the past.
    auto atFirstWriteResponse = unittest::assertGet(
        runFindQuery(nss,
                     BSONObj(),
                     BSON("fooItem" << 1),
                     boost::none,
                     repl::ReadConcernArgs::snapshot(opTimeAfterFirstWrite.getTimestamp())));
    ASSERT_EQUALS(1U, atFirstWriteResponse.docs.size());
    ASSERT_EQUALS(1, atFirstWriteResponse.docs[0]["fooItem"].numberInt());
}

TEST_F(ShardLocalTest, QueryRestoresOriginalReadSource) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");
    ASSERT_OK(runFindAndModifyRunCommand(
                  nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)))
                  .getStatus());
    const auto atClusterTime =
        getReplCoordMock()->getCurrentCommittedSnapshotOpTime().getTimestamp();

    const auto originalReadSource =
        shard_role_details::getRecoveryUnit(_opCtx.get())->getTimestampReadSource();

    for (const auto& readConcern : {repl::ReadConcernArgs::kMajority,
                                    repl::ReadConcernArgs::kSnapshot,
                                    repl::ReadConcernArgs::snapshot(atClusterTime)}) {
        ASSERT_OK(runFindQuery(nss, BSONObj(), BSON("fooItem" << 1), boost::none, readConcern)
                      .getStatus());
        ASSERT_EQUALS(originalReadSource,
                      shard_role_details::getRecoveryUnit(_opCtx.get())->getTimestampReadSource());
    }
}

}  // namespace
}  // namespace mongo
