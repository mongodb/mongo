/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/initialize_operation_session_info.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/thread.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using repl::OplogEntry;
using repl::OpTime;
using repl::OpTypeEnum;
using unittest::assertGet;

const ConnectionString kConfigConnStr =
    ConnectionString::forReplicaSet("config", {HostAndPort("config:1")});

const ConnectionString kDonorConnStr =
    ConnectionString::forReplicaSet("donor", {HostAndPort("donor:1")});
const ShardId kFromShard("donor");
const NamespaceString kNs("a.b");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(
    repl::OpTime opTime,
    repl::OpTypeEnum opType,
    BSONObj object,
    boost::optional<BSONObj> object2,
    OperationSessionInfo sessionInfo,
    Date_t wallClockTime,
    const std::vector<StmtId>& stmtIds,
    boost::optional<repl::OpTime> preImageOpTime = boost::none,
    boost::optional<repl::OpTime> postImageOpTime = boost::none,
    boost::optional<repl::RetryImageEnum> needsRetryImage = boost::none) {
    return {
        repl::DurableOplogEntry(opTime,          // optime
                                0,               // hash
                                opType,          // opType
                                kNs,             // namespace
                                boost::none,     // uuid
                                boost::none,     // fromMigrate
                                0,               // version
                                object,          // o
                                object2,         // o2
                                sessionInfo,     // sessionInfo
                                boost::none,     // isUpsert
                                wallClockTime,   // wall clock time
                                stmtIds,         // statement ids
                                boost::none,     // optime of previous write within same transaction
                                preImageOpTime,  // pre-image optime
                                postImageOpTime,  // post-image optime
                                boost::none,      // ShardId of resharding recipient
                                boost::none,      // _id
                                needsRetryImage)};
}

repl::OplogEntry extractInnerOplog(const repl::OplogEntry& oplog) {
    ASSERT_TRUE(oplog.getObject2());

    auto oplogStatus = repl::OplogEntry::parse(oplog.getObject2().value());
    ASSERT_OK(oplogStatus);
    return oplogStatus.getValue();
}

class SessionCatalogMigrationDestinationTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        _migrationId = MigrationSessionId::generate("donor", "recipient");

        {
            auto donorShard = assertGet(
                shardRegistry()->getShard(operationContext(), kDonorConnStr.getSetName()));
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setConnectionStringReturnValue(kDonorConnStr);
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setFindHostReturnValue(kDonorConnStr.getServers()[0]);
        }
        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::set(getServiceContext(), std::make_unique<MongoDSessionCatalog>());
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
        mongoDSessionCatalog->onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        setUnshardedFilteringMetadata(kNs);
    }

    void returnOplog(const std::vector<OplogEntry>& oplogList) {
        onCommand([&oplogList](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            BSONObjBuilder builder;
            BSONArrayBuilder arrBuilder(builder.subarrayStart("oplog"));

            for (const auto& oplog : oplogList) {
                arrBuilder.append(oplog.getEntry().toBSON());
            }

            arrBuilder.doneFast();
            return builder.obj();
        });
    }

    repl::OplogEntry getOplog(OperationContext* opCtx, const repl::OpTime& opTime) {
        DBDirectClient client(opCtx);
        auto oplogBSON = client.findOne(NamespaceString::kRsOplogNamespace, opTime.asQuery());

        ASSERT_FALSE(oplogBSON.isEmpty());
        auto parseStatus = repl::OplogEntry::parse(oplogBSON);
        ASSERT_OK(parseStatus);

        return parseStatus.getValue();
    }

    MigrationSessionId migrationId() {
        return _migrationId.value();
    }

    void setUpSessionWithTxn(OperationContext* opCtx,
                             const LogicalSessionId& sessionId,
                             const TxnNumber& txnNum) {
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, {txnNum}, boost::none /* autocommit */, boost::none /* startTransaction */);
    }

    void checkOplog(const repl::OplogEntry& originalOplog, const repl::OplogEntry& oplogToCheck) {
        _checkOplogExceptO2(originalOplog, oplogToCheck);
        ASSERT_BSONOBJ_EQ(*originalOplog.getObject2(), *oplogToCheck.getObject2());
    }

    void checkOplogWithNestedOplog(const repl::OplogEntry& originalOplog,
                                   const repl::OplogEntry& oplogToCheck) {
        _checkOplogExceptO2(originalOplog, oplogToCheck);

        auto innerOplog = extractInnerOplog(oplogToCheck);
        ASSERT_TRUE(innerOplog.getOpType() == originalOplog.getOpType());
        ASSERT_BSONOBJ_EQ(originalOplog.getObject(), innerOplog.getObject());

        if (originalOplog.getObject2()) {
            ASSERT_TRUE(innerOplog.getObject2());
            ASSERT_BSONOBJ_EQ(*originalOplog.getObject2(), *innerOplog.getObject2());
        } else {
            ASSERT_FALSE(innerOplog.getObject2());
        }
    }

    void checkStatementExecuted(OperationContext* opCtx, TxnNumber txnNumber, StmtId stmtId) {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        auto oplog = txnParticipant.checkStatementExecuted(opCtx, stmtId);
        ASSERT_TRUE(oplog);
    }

    void checkStatementExecuted(OperationContext* opCtx,
                                TxnNumber txnNumber,
                                StmtId stmtId,
                                const repl::OplogEntry& expectedOplog) {
        const auto txnParticipant = TransactionParticipant::get(opCtx);
        auto oplog = txnParticipant.checkStatementExecuted(opCtx, stmtId);
        ASSERT_TRUE(oplog);
        checkOplogWithNestedOplog(expectedOplog, *oplog);
    }

    void insertDocWithSessionInfo(const OperationSessionInfo& sessionInfo,
                                  const NamespaceString& ns,
                                  const BSONObj& doc,
                                  StmtId stmtId) {
        // Do write on a separate thread in order not to pollute this thread's opCtx.
        stdx::thread insertThread([sessionInfo, ns, doc, stmtId] {
            write_ops::WriteCommandRequestBase cmdBase;
            std::vector<StmtId> stmtIds;
            stmtIds.push_back(stmtId);
            cmdBase.setStmtIds(stmtIds);

            write_ops::InsertCommandRequest insertRequest(ns);
            std::vector<BSONObj> documents;
            documents.push_back(doc);
            insertRequest.setDocuments(documents);
            insertRequest.setWriteCommandRequestBase(cmdBase);

            BSONObjBuilder insertBuilder;
            insertRequest.serialize({}, &insertBuilder);
            sessionInfo.serialize(&insertBuilder);

            Client::initThread("test-insert-thread");
            auto innerOpCtx = Client::getCurrent()->makeOperationContext();

            initializeOperationSessionInfo(innerOpCtx.get(), insertBuilder.obj(), true, true, true);
            MongoDOperationContextSession sessionTxnState(innerOpCtx.get());
            auto txnParticipant = TransactionParticipant::get(innerOpCtx.get());
            txnParticipant.beginOrContinue(innerOpCtx.get(),
                                           {*sessionInfo.getTxnNumber()},
                                           boost::none /* autocommit */,
                                           boost::none /* startTransaction */);

            const auto reply = write_ops_exec::performInserts(innerOpCtx.get(), insertRequest);
            ASSERT(reply.results.size() == 1);
            ASSERT(reply.results[0].isOK());
        });

        insertThread.join();
    }

    void finishSessionExpectSuccess(SessionCatalogMigrationDestination* sessionMigration) {
        sessionMigration->finish();
        // migration always fetches at least twice to transition from committing to done.
        returnOplog({});
        returnOplog({});

        sessionMigration->join();

        ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done ==
                    sessionMigration->getState());
    }

    void setUnshardedFilteringMetadata(const NamespaceString& nss) {
        AutoGetDb autoDb(operationContext(), nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), nss, MODE_IX);
        CollectionShardingRuntime::get(operationContext(), nss)
            ->setFilteringMetadata(operationContext(), CollectionMetadata());
    }

    CancellationSource _cancellationSource;

private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() = default;

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {

                ShardType donorShard;
                donorShard.setName(kDonorConnStr.getSetName());
                donorShard.setHost(kDonorConnStr.toString());

                return repl::OpTimeWith<std::vector<ShardType>>({donorShard});
            }
        };

        return std::make_unique<StaticCatalogClient>();
    }

    void _checkOplogExceptO2(const repl::OplogEntry& originalOplog,
                             const repl::OplogEntry& oplogToCheck) {
        ASSERT_TRUE(oplogToCheck.getOpType() == OpTypeEnum::kNoop);

        auto originalStmtIds = originalOplog.getStatementIds();
        auto checkStmtIds = oplogToCheck.getStatementIds();
        ASSERT_EQ(originalStmtIds.size(), checkStmtIds.size());
        for (size_t i = 0; i < originalStmtIds.size(); ++i) {
            ASSERT_EQ(originalStmtIds[i], checkStmtIds[i]);
        }

        const auto origSessionInfo = originalOplog.getOperationSessionInfo();
        const auto sessionInfoToCheck = oplogToCheck.getOperationSessionInfo();

        ASSERT_TRUE(sessionInfoToCheck.getSessionId());
        ASSERT_EQ(*origSessionInfo.getSessionId(), *sessionInfoToCheck.getSessionId());

        ASSERT_TRUE(sessionInfoToCheck.getTxnNumber());
        ASSERT_EQ(*origSessionInfo.getTxnNumber(), *sessionInfoToCheck.getTxnNumber());

        ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, oplogToCheck.getObject());
    }

    boost::optional<MigrationSessionId> _migrationId;
};

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyWhenNothingToTransfer) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());
    finishSessionExpectSuccess(&sessionMigration);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithSameTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // sessionInfo
                                 Date_t::now(),                // wall clock time
                                 {5});                         // statement ids

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);
    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 2, 23, oplog1);
    checkStatementExecuted(opCtx, 2, 45, oplog2);
    checkStatementExecuted(opCtx, 2, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithMultiStmtIds) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23, 24});                     // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45, 46});                    // statement ids

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // sessionInfo
                                 Date_t::now(),                // wall clock time
                                 {5, 6});                      // statement ids

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);
    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 2, 23, oplog1);
    checkStatementExecuted(opCtx, 2, 24, oplog1);
    checkStatementExecuted(opCtx, 2, 45, oplog2);
    checkStatementExecuted(opCtx, 2, 46, oplog2);
    checkStatementExecuted(opCtx, 2, 5, oplog3);
    checkStatementExecuted(opCtx, 2, 6, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldOnlyStoreHistoryOfLatestTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    TxnNumber txnNum(2);

    sessionInfo.setTxnNumber(txnNum++);
    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    sessionInfo.setTxnNumber(txnNum++);
    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    sessionInfo.setTxnNumber(txnNum);
    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {5});                         // statement ids

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, txnNum);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));
    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, txnNum, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithSameTxnInSeparateBatches) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {5});                         // statement ids

    // Return in 2 batches
    returnOplog({oplog1, oplog2});
    returnOplog({oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 2, 23, oplog1);
    checkStatementExecuted(opCtx, 2, 45, oplog2);
    checkStatementExecuted(opCtx, 2, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithDifferentSession) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto sessionId2 = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo1;
    sessionInfo1.setSessionId(sessionId1);
    sessionInfo1.setTxnNumber(2);

    OperationSessionInfo sessionInfo2;
    sessionInfo2.setSessionId(sessionId2);
    sessionInfo2.setTxnNumber(42);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo1,                  // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo2,                 // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo2,                 // session info
                                 Date_t::now(),                // wall clock time
                                 {5});                         // statement ids

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();

    {
        setUpSessionWithTxn(opCtx, sessionId1, 2);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));
        ASSERT_FALSE(historyIter.hasNext());

        checkStatementExecuted(opCtx, 2, 23, oplog1);
    }

    {
        // XXX TODO USE A DIFFERENT OPERATION CONTEXT!
        auto client2 = getServiceContext()->makeClient("client2");
        AlternativeClientRegion acr(client2);
        auto opCtx2 = cc().makeOperationContext();
        setUpSessionWithTxn(opCtx2.get(), sessionId2, 42);
        MongoDOperationContextSession ocs(opCtx2.get());
        auto txnParticipant = TransactionParticipant::get(opCtx2.get());


        TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx2.get()));

        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx2.get()));

        ASSERT_FALSE(historyIter.hasNext());

        checkStatementExecuted(opCtx2.get(), 42, 45, oplog2);
        checkStatementExecuted(opCtx2.get(), 42, 5, oplog3);
    }
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldNotNestAlreadyNestedOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto origInnerOplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                          OpTypeEnum::kInsert,           // op type
                                          BSON("x" << 100),              // o
                                          boost::none,                   // o2
                                          sessionInfo,                   // session info
                                          Date_t(),                      // wall clock time
                                          {23});                         // statement ids

    auto origInnerOplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                          OpTypeEnum::kInsert,          // op type
                                          BSON("x" << 80),              // o
                                          boost::none,                  // o2
                                          sessionInfo,                  // session info
                                          Date_t(),                     // wall clock time
                                          {45});                        // statement ids

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(1100, 2), 1),        // optime
                                 OpTypeEnum::kNoop,                    // op type
                                 BSONObj(),                            // o
                                 origInnerOplog1.getEntry().toBSON(),  // o2
                                 sessionInfo,                          // session info
                                 Date_t::now(),                        // wall clock time
                                 {23});                                // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(1080, 2), 1),        // optime
                                 OpTypeEnum::kNoop,                    // op type
                                 BSONObj(),                            // o
                                 origInnerOplog2.getEntry().toBSON(),  // o2
                                 sessionInfo,                          // session info
                                 Date_t::now(),                        // wall clock time
                                 {45});                                // statement ids

    returnOplog({oplog1, oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 2, 23);
    checkStatementExecuted(opCtx, 2, 45);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandlePreImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      {45},                                // statement ids
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);                        // post-image optime

    returnOplog({preImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);


    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, nextOplog.getStatementIds().front());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, nextOplog.getObject());

    auto innerOplog = extractInnerOplog(nextOplog);
    ASSERT_TRUE(innerOplog.getOpType() == OpTypeEnum::kUpdate);
    ASSERT_BSONOBJ_EQ(updateOplog.getObject(), innerOplog.getObject());
    ASSERT_TRUE(innerOplog.getObject2());
    ASSERT_BSONOBJ_EQ(updateOplog.getObject2().value(), innerOplog.getObject2().value());

    ASSERT_FALSE(historyIter.hasNext());

    ASSERT_TRUE(nextOplog.getPreImageOpTime());
    ASSERT_FALSE(nextOplog.getPostImageOpTime());

    // Check preImage oplog

    auto preImageOpTime = nextOplog.getPreImageOpTime().value();
    auto newPreImageOplog = getOplog(opCtx, preImageOpTime);

    ASSERT_EQ(1, newPreImageOplog.getStatementIds().size());
    ASSERT_EQ(45, newPreImageOplog.getStatementIds().front());

    auto preImageSessionInfo = newPreImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(preImageOplog.getObject(), newPreImageOplog.getObject());
    ASSERT_TRUE(newPreImageOplog.getObject2());
    ASSERT_TRUE(newPreImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandleForgedPreImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement id
    // Test that we will downconvert the updateOplog by removing the 'needsRetryImage' and appending
    // the proper preImageOpTime link.
    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),       // optime
                                      OpTypeEnum::kUpdate,               // op type
                                      BSON("x" << 100),                  // o
                                      BSON("$set" << BSON("x" << 101)),  // o2
                                      sessionInfo,                       // session info
                                      Date_t::now(),                     // wall clock time
                                      {45},                              // statement id
                                      boost::none,                       // pre-image optime
                                      boost::none,                       // post-image optime
                                      repl::RetryImageEnum::kPreImage);  // needsRetryImage

    returnOplog({preImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, nextOplog.getStatementIds().front());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, nextOplog.getObject());

    auto innerOplog = extractInnerOplog(nextOplog);
    ASSERT_TRUE(innerOplog.getOpType() == OpTypeEnum::kUpdate);
    ASSERT_BSONOBJ_EQ(updateOplog.getObject(), innerOplog.getObject());
    ASSERT_TRUE(innerOplog.getObject2());
    ASSERT_BSONOBJ_EQ(updateOplog.getObject2().value(), innerOplog.getObject2().value());

    ASSERT_FALSE(historyIter.hasNext());

    ASSERT_TRUE(nextOplog.getPreImageOpTime());
    ASSERT_FALSE(nextOplog.getPostImageOpTime());
    ASSERT_FALSE(nextOplog.getNeedsRetryImage());

    // Check preImage oplog

    auto preImageOpTime = nextOplog.getPreImageOpTime().value();
    auto newPreImageOplog = getOplog(opCtx, preImageOpTime);

    ASSERT_EQ(1, newPreImageOplog.getStatementIds().size());
    ASSERT_EQ(45, newPreImageOplog.getStatementIds().front());

    auto preImageSessionInfo = newPreImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(preImageOplog.getObject(), newPreImageOplog.getObject());
    ASSERT_TRUE(newPreImageOplog.getObject2());
    ASSERT_TRUE(newPreImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandlePostImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto postImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                         OpTypeEnum::kNoop,             // op type
                                         BSON("x" << 100),              // o
                                         boost::none,                   // o2
                                         sessionInfo,                   // session info
                                         Date_t::now(),                 // wall clock time
                                         {45});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                      OpTypeEnum::kUpdate,          // op type
                                      BSON("x" << 100),             // o
                                      BSON("$set" << BSON("x" << 101)),
                                      sessionInfo,                          // session info
                                      Date_t::now(),                        // wall clock time
                                      {45},                                 // statement ids
                                      boost::none,                          // pre-image optime
                                      repl::OpTime(Timestamp(100, 2), 1));  // post-image optime

    returnOplog({postImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, nextOplog.getStatementIds().front());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, nextOplog.getObject());

    auto innerOplog = extractInnerOplog(nextOplog);
    ASSERT_TRUE(innerOplog.getOpType() == OpTypeEnum::kUpdate);
    ASSERT_BSONOBJ_EQ(updateOplog.getObject(), innerOplog.getObject());
    ASSERT_TRUE(innerOplog.getObject2());
    ASSERT_BSONOBJ_EQ(updateOplog.getObject2().value(), innerOplog.getObject2().value());

    ASSERT_FALSE(historyIter.hasNext());

    ASSERT_FALSE(nextOplog.getPreImageOpTime());
    ASSERT_TRUE(nextOplog.getPostImageOpTime());

    // Check preImage oplog

    auto postImageOpTime = nextOplog.getPostImageOpTime().value();
    auto newPostImageOplog = getOplog(opCtx, postImageOpTime);

    ASSERT_EQ(1, newPostImageOplog.getStatementIds().size());
    ASSERT_EQ(45, newPostImageOplog.getStatementIds().front());

    auto preImageSessionInfo = newPostImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(postImageOplog.getObject(), newPostImageOplog.getObject());
    ASSERT_TRUE(newPostImageOplog.getObject2());
    ASSERT_TRUE(newPostImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, 2, 45, updateOplog);
}


TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandleForgedPostImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto postImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                         OpTypeEnum::kNoop,             // op type
                                         BSON("x" << 100),              // o
                                         boost::none,                   // o2
                                         sessionInfo,                   // session info
                                         Date_t::now(),                 // wall clock time
                                         {45});                         // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                      OpTypeEnum::kUpdate,          // op type
                                      BSON("x" << 100),             // o
                                      BSON("$set" << BSON("x" << 101)),
                                      sessionInfo,                        // session info
                                      Date_t::now(),                      // wall clock time
                                      {45},                               // statement id
                                      boost::none,                        // pre-image optime
                                      boost::none,                        // post-image optime
                                      repl::RetryImageEnum::kPostImage);  // needsRetryImage

    returnOplog({postImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, nextOplog.getStatementIds().front());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, nextOplog.getObject());

    auto innerOplog = extractInnerOplog(nextOplog);
    ASSERT_TRUE(innerOplog.getOpType() == OpTypeEnum::kUpdate);
    ASSERT_BSONOBJ_EQ(updateOplog.getObject(), innerOplog.getObject());
    ASSERT_TRUE(innerOplog.getObject2());
    ASSERT_BSONOBJ_EQ(updateOplog.getObject2().value(), innerOplog.getObject2().value());

    ASSERT_FALSE(historyIter.hasNext());

    ASSERT_FALSE(nextOplog.getPreImageOpTime());
    ASSERT_TRUE(nextOplog.getPostImageOpTime());
    ASSERT_FALSE(nextOplog.getNeedsRetryImage());

    // Check preImage oplog

    auto postImageOpTime = nextOplog.getPostImageOpTime().value();
    auto newPostImageOplog = getOplog(opCtx, postImageOpTime);

    ASSERT_EQ(1, newPostImageOplog.getStatementIds().size());
    ASSERT_EQ(45, newPostImageOplog.getStatementIds().front());

    auto preImageSessionInfo = newPostImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(postImageOplog.getObject(), newPostImageOplog.getObject());
    ASSERT_TRUE(newPostImageOplog.getObject2());
    ASSERT_TRUE(newPostImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandleFindAndModifySplitIn2Batches) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      {45},                                // statement ids
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);

    returnOplog({preImageOplog});
    returnOplog({updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);


    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, nextOplog.getStatementIds().front());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(SessionCatalogMigration::kSessionOplogTag, nextOplog.getObject());

    auto innerOplog = extractInnerOplog(nextOplog);
    ASSERT_TRUE(innerOplog.getOpType() == OpTypeEnum::kUpdate);
    ASSERT_BSONOBJ_EQ(updateOplog.getObject(), innerOplog.getObject());
    ASSERT_TRUE(innerOplog.getObject2());
    ASSERT_BSONOBJ_EQ(updateOplog.getObject2().value(), innerOplog.getObject2().value());

    ASSERT_FALSE(historyIter.hasNext());

    ASSERT_TRUE(nextOplog.getPreImageOpTime());
    ASSERT_FALSE(nextOplog.getPostImageOpTime());

    // Check preImage oplog

    auto preImageOpTime = nextOplog.getPreImageOpTime().value();
    auto newPreImageOplog = getOplog(opCtx, preImageOpTime);

    ASSERT_EQ(1, nextOplog.getStatementIds().size());
    ASSERT_EQ(45, newPreImageOplog.getStatementIds().front());

    auto preImageSessionInfo = newPreImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(preImageOplog.getObject(), newPreImageOplog.getObject());
    ASSERT_TRUE(newPreImageOplog.getObject2());
    ASSERT_TRUE(newPreImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, OlderTxnShouldBeIgnored) {
    const auto sessionId = makeLogicalSessionIdForTest();

    auto opCtx = operationContext();

    OperationSessionInfo newSessionInfo;
    newSessionInfo.setSessionId(sessionId);
    newSessionInfo.setTxnNumber(20);

    insertDocWithSessionInfo(newSessionInfo,
                             kNs,
                             BSON("_id"
                                  << "newerSess"),
                             0);

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo oldSessionInfo;
    oldSessionInfo.setSessionId(sessionId);
    oldSessionInfo.setTxnNumber(19);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 oldSessionInfo,                // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 oldSessionInfo,               // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    returnOplog({oplog1, oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    setUpSessionWithTxn(opCtx, sessionId, 20);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);


    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());
    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 20, 0);
}

TEST_F(SessionCatalogMigrationDestinationTest, NewerTxnWriteShouldNotBeOverwrittenByOldMigrateTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    auto opCtx = operationContext();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo oldSessionInfo;
    oldSessionInfo.setSessionId(sessionId);
    oldSessionInfo.setTxnNumber(19);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 oldSessionInfo,
                                 Date_t::now(),  // wall clock time
                                 {23});          // statement ids

    returnOplog({oplog1});

    OperationSessionInfo newSessionInfo;
    newSessionInfo.setSessionId(sessionId);
    newSessionInfo.setTxnNumber(20);

    // Ensure that the previous oplog has been processed before proceeding.
    returnOplog({});

    insertDocWithSessionInfo(newSessionInfo,
                             kNs,
                             BSON("_id"
                                  << "newerSess"),
                             0);

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 oldSessionInfo,               // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    returnOplog({oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    setUpSessionWithTxn(opCtx, sessionId, 20);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());
    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    checkStatementExecuted(opCtx, 20, 0);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyAfterNetworkError) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::SocketException, "Bad connection"};
    });

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoOplog) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return BSON("oplog" << 1);
    });

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithBadOplogFormat) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    onCommand([](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        BSONObjBuilder builder;
        BSONArrayBuilder arrBuilder(builder.subarrayStart("oplog"));
        arrBuilder.append(BSON("x" << 1));
        arrBuilder.doneFast();
        return builder.obj();
    });

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoSessionId) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setTxnNumber(2);

    auto oplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                OpTypeEnum::kInsert,           // op type
                                BSON("x" << 100),              // o
                                boost::none,                   // o2
                                sessionInfo,                   // session info
                                Date_t::now(),                 // wall clock time
                                {23});                         // statement ids

    returnOplog({oplog});

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoTxnNumber) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());

    auto oplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                OpTypeEnum::kInsert,           // op type
                                BSON("x" << 100),              // o
                                boost::none,                   // o2
                                sessionInfo,                   // session info
                                Date_t::now(),                 // wall clock time
                                {23});                         // statement ids

    returnOplog({oplog});

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoStmtId) {
    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo.setTxnNumber(2);

    auto oplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                OpTypeEnum::kInsert,           // op type
                                BSON("x" << 100),              // o
                                boost::none,                   // o2
                                sessionInfo,                   // session info
                                Date_t::now(),                 // wall clock time
                                {});                           // statement ids

    returnOplog({oplog});

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       NewWritesWithSameTxnDuringMigrationShouldBeCorrectlySet) {
    const auto sessionId = makeLogicalSessionIdForTest();
    auto opCtx = operationContext();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    returnOplog({oplog1});

    // Ensure that the previous oplog has been processed before proceeding.
    returnOplog({});

    insertDocWithSessionInfo(sessionInfo,
                             kNs,
                             BSON("_id"
                                  << "newerSess"),
                             0);

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    returnOplog({oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    setUpSessionWithTxn(opCtx, sessionId, 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);


    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, 2, 0);
    checkStatementExecuted(opCtx, 2, 23, oplog1);
    checkStatementExecuted(opCtx, 2, 45, oplog2);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldErrorForConsecutivePreImageOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                         OpTypeEnum::kNoop,             // op type
                                         BSON("x" << 100),              // o
                                         boost::none,                   // o2
                                         sessionInfo,                   // session info
                                         Date_t::now(),                 // wall clock time
                                         {45});                         // statement ids

    auto preImageOplog2 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                         OpTypeEnum::kNoop,             // op type
                                         BSON("x" << 100),              // o
                                         boost::none,                   // o2
                                         sessionInfo,                   // session info
                                         Date_t::now(),                 // wall clock time
                                         {45});                         // statement ids

    returnOplog({preImageOplog1, preImageOplog2});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorForPreImageOplogWithNonMatchingSessionId) {

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement ids

    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                      OpTypeEnum::kUpdate,          // op type
                                      BSON("x" << 100),             // o
                                      BSON("$set" << BSON("x" << 101)),
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      {45},                                // statement ids
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);                        // post-image optime

    returnOplog({preImageOplog, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldErrorForPreImageOplogWithNonMatchingTxnNum) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement ids

    sessionInfo.setTxnNumber(56);
    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      {45},                                // statement ids
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);                        // post-image optime

    returnOplog({preImageOplog, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorIfPreImageOplogFollowWithOplogWithNoPreImageLink) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto preImageOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kNoop,             // op type
                                        BSON("x" << 100),              // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {45});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),       // optime
                                      OpTypeEnum::kUpdate,               // op type
                                      BSON("x" << 100),                  // o
                                      BSON("$set" << BSON("x" << 101)),  // o2
                                      sessionInfo,                       // session info
                                      Date_t::now(),                     // wall clock time
                                      {45});                             // statement ids

    returnOplog({preImageOplog, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorIfOplogWithPreImageLinkIsPrecededByNormalOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      {45},                                // statement ids
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);                        // post-image optime

    returnOplog({oplog1, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorIfOplogWithPostImageLinkIsPrecededByNormalOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());

    sessionMigration.start(getServiceContext());

    sessionMigration.finish();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                 OpTypeEnum::kInsert,           // op type
                                 BSON("x" << 100),              // o
                                 boost::none,                   // o2
                                 sessionInfo,                   // session info
                                 Date_t::now(),                 // wall clock time
                                 {23});                         // statement ids

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),          // optime
                                      OpTypeEnum::kUpdate,                  // op type
                                      BSON("x" << 100),                     // o
                                      BSON("$set" << BSON("x" << 101)),     // o2
                                      sessionInfo,                          // session info
                                      Date_t::now(),                        // wall clock time
                                      {45},                                 // statement ids
                                      boost::none,                          // pre-image optime
                                      repl::OpTime(Timestamp(100, 2), 1));  // post-image optime

    returnOplog({oplog1, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldIgnoreAlreadyExecutedStatements) {
    const auto sessionId = makeLogicalSessionIdForTest();

    auto opCtx = operationContext();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(19);

    insertDocWithSessionInfo(sessionInfo, kNs, BSON("_id" << 46), 30);

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 100),             // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {23});                        // statement ids

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(70, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {30});                        // statement ids

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 {45});                        // statement ids

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    setUpSessionWithTxn(opCtx, sessionId, 19);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());
    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    auto firstInsertOplog = historyIter.next(opCtx);

    ASSERT_TRUE(firstInsertOplog.getOpType() == OpTypeEnum::kInsert);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 46), firstInsertOplog.getObject());
    ASSERT_EQ(1, firstInsertOplog.getStatementIds().size());
    ASSERT_EQ(30, firstInsertOplog.getStatementIds().front());

    checkStatementExecuted(opCtx, 19, 23, oplog1);
    checkStatementExecuted(opCtx, 19, 30);
    checkStatementExecuted(opCtx, 19, 45, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithIncompleteHistory) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo.setTxnNumber(2);
    const std::vector<repl::OplogEntry> oplogEntries{
        makeOplogEntry(OpTime(Timestamp(100, 2), 1),              // optime
                       OpTypeEnum::kInsert,                       // op type
                       BSON("x" << 100),                          // o
                       boost::none,                               // o2
                       sessionInfo,                               // session info
                       Date_t::now(),                             // wall clock time
                       {23}),                                     // statement ids
        makeOplogEntry(OpTime(Timestamp(80, 2), 1),               // optime
                       OpTypeEnum::kNoop,                         // op type
                       {},                                        // o
                       TransactionParticipant::kDeadEndSentinel,  // o2
                       sessionInfo,                               // session info
                       Date_t::now(),                             // wall clock time
                       {kIncompleteHistoryStmtId}),               // statement ids
        // This will get ignored since previous entry will make the history 'incomplete'.
        makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                       OpTypeEnum::kInsert,          // op type
                       BSON("x" << 60),              // o
                       boost::none,                  // o2
                       sessionInfo,                  // session info
                       Date_t::now(),                // wall clock time
                       {5})};                        // statement ids

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    returnOplog(oplogEntries);

    // migration always fetches at least twice to transition from committing to done.
    returnOplog({});
    returnOplog({});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();
    setUpSessionWithTxn(opCtx, *sessionInfo.getSessionId(), 2);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    TransactionHistoryIterator historyIter(txnParticipant.getLastWriteOpTime());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplogEntries[1], historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplogEntries[0], historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       OplogEntriesWithOldTransactionFollowedByUpToDateEntries) {
    auto opCtx = operationContext();

    OperationSessionInfo sessionInfo1;
    sessionInfo1.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo1.setTxnNumber(2);
    {
        // "Start" a new transaction on session 1, so that migrating the entries above will result
        // in TransactionTooOld. This should not preclude the entries for session 2 from getting
        // applied.
        setUpSessionWithTxn(opCtx, *sessionInfo1.getSessionId(), *sessionInfo1.getTxnNumber());
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        txnParticipant.refreshFromStorageIfNeeded(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, {3}, boost::none /* autocommit */, boost::none /* startTransaction */);
    }

    OperationSessionInfo sessionInfo2;
    sessionInfo2.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo2.setTxnNumber(15);

    const std::vector<repl::OplogEntry> oplogEntries{
        // Session 1 entries
        makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                       OpTypeEnum::kInsert,           // op type
                       BSON("x" << 100),              // o
                       boost::none,                   // o2
                       sessionInfo1,                  // session info
                       Date_t::now(),                 // wall clock time
                       {23}),                         // statement ids

        // Session 2 entries
        makeOplogEntry(OpTime(Timestamp(50, 2), 1),  // optime
                       OpTypeEnum::kInsert,          // op type
                       BSON("x" << 50),              // o
                       boost::none,                  // o2
                       sessionInfo2,                 // session info
                       Date_t::now(),                // wall clock time
                       {56}),                        // statement ids
        makeOplogEntry(OpTime(Timestamp(20, 2), 1),  // optime
                       OpTypeEnum::kInsert,          // op type
                       BSON("x" << 20),              // o
                       boost::none,                  // o2
                       sessionInfo2,                 // session info
                       Date_t::now(),                // wall clock time
                       {55})};                       // statement ids

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    returnOplog(oplogEntries);

    // migration always fetches at least twice to transition from committing to done.
    returnOplog({});
    returnOplog({});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    // Check nothing was written for session 1
    {
        auto c1 = getServiceContext()->makeClient("c1");
        AlternativeClientRegion acr(c1);
        auto opCtx1 = cc().makeOperationContext();
        auto opCtx = opCtx1.get();
        setUpSessionWithTxn(opCtx, *sessionInfo1.getSessionId(), 3);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant1 = TransactionParticipant::get(opCtx);
        ASSERT(txnParticipant1.getLastWriteOpTime().isNull());
    }

    // Check session 2 was correctly updated
    {
        auto c2 = getServiceContext()->makeClient("c2");
        AlternativeClientRegion acr(c2);
        auto opCtx2 = cc().makeOperationContext();
        auto opCtx = opCtx2.get();
        setUpSessionWithTxn(opCtx, *sessionInfo2.getSessionId(), 15);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant2 = TransactionParticipant::get(opCtx);

        TransactionHistoryIterator historyIter(txnParticipant2.getLastWriteOpTime());
        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplogEntries[2], historyIter.next(opCtx));

        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplogEntries[1], historyIter.next(opCtx));

        ASSERT_FALSE(historyIter.hasNext());

        checkStatementExecuted(opCtx, 15, 56, oplogEntries[1]);
        checkStatementExecuted(opCtx, 15, 55, oplogEntries[2]);
    }
}

TEST_F(SessionCatalogMigrationDestinationTest, MigratingKnownStmtWhileOplogTruncated) {
    auto opCtx = operationContext();
    const StmtId kStmtId = 45;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo.setTxnNumber(19);

    insertDocWithSessionInfo(sessionInfo, kNs, BSON("_id" << 46), kStmtId);

    auto getLastWriteOpTime = [&]() {
        auto c1 = getServiceContext()->makeClient("c1");
        AlternativeClientRegion acr(c1);
        auto innerOpCtx = cc().makeOperationContext();
        setUpSessionWithTxn(innerOpCtx.get(), *sessionInfo.getSessionId(), 19);
        MongoDOperationContextSession ocs(innerOpCtx.get());
        auto txnParticipant = TransactionParticipant::get(innerOpCtx.get());
        return txnParticipant.getLastWriteOpTime();
    };

    auto lastOpTimeBeforeMigrate = getLastWriteOpTime();

    {
        // Empty the oplog collection.
        AutoGetCollection oplogColl(opCtx, NamespaceString::kRsOplogNamespace, MODE_X);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(oplogColl.getWritableCollection(opCtx)->truncate(opCtx));
        wuow.commit();
    }

    {
        // Confirm that oplog is indeed empty.
        DBDirectClient client(opCtx);
        auto result = client.findOne(NamespaceString::kRsOplogNamespace, BSONObj{});
        ASSERT_TRUE(result.isEmpty());
    }

    auto sameStmtOplog = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                        OpTypeEnum::kInsert,           // op type
                                        BSON("_id" << 46),             // o
                                        boost::none,                   // o2
                                        sessionInfo,                   // session info
                                        Date_t::now(),                 // wall clock time
                                        {kStmtId});                    // statement ids

    SessionCatalogMigrationDestination sessionMigration(
        kNs, kFromShard, migrationId(), _cancellationSource.token());
    sessionMigration.start(getServiceContext());
    sessionMigration.finish();

    returnOplog({sameStmtOplog});

    // migration always fetches at least twice to transition from committing to done.
    returnOplog({});
    returnOplog({});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    ASSERT_EQ(lastOpTimeBeforeMigrate, getLastWriteOpTime());
}

}  // namespace
}  // namespace mongo
