/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_gen.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_mongod_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

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

const BSONObj kSessionOplogTag(BSON(SessionCatalogMigrationDestination::kSessionMigrateOplogTag
                                    << 1));

const NamespaceString kNs("a.b");

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                BSONObj object,
                                boost::optional<BSONObj> object2,
                                OperationSessionInfo sessionInfo,
                                boost::optional<Date_t> wallClockTime,
                                boost::optional<StmtId> stmtId,
                                boost::optional<repl::OpTime> preImageOpTime = boost::none,
                                boost::optional<repl::OpTime> postImageOpTime = boost::none) {
    return repl::OplogEntry(opTime,            // optime
                            0,                 // hash
                            opType,            // opType
                            kNs,               // namespace
                            boost::none,       // uuid
                            boost::none,       // fromMigrate
                            0,                 // version
                            object,            // o
                            object2,           // o2
                            sessionInfo,       // sessionInfo
                            boost::none,       // isUpsert
                            wallClockTime,     // wall clock time
                            stmtId,            // statement id
                            boost::none,       // optime of previous write within same transaction
                            preImageOpTime,    // pre-image optime
                            postImageOpTime);  // post-image optime
}

repl::OplogEntry extractInnerOplog(const repl::OplogEntry& oplog) {
    ASSERT_TRUE(oplog.getObject2());

    auto oplogStatus = repl::OplogEntry::parse(oplog.getObject2().value());
    ASSERT_OK(oplogStatus);
    return oplogStatus.getValue();
}

class SessionCatalogMigrationDestinationTest : public ShardingMongodTestFixture {
public:
    void setUp() override {
        serverGlobalParams.featureCompatibility.setVersion(
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        ShardingMongodTestFixture::setUp();

        ASSERT_OK(initializeGlobalShardingStateForMongodForTest(kConfigConnStr));

        RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter())
            ->setConnectionStringReturnValue(kConfigConnStr);

        {
            auto donorShard = assertGet(
                shardRegistry()->getShard(operationContext(), kDonorConnStr.getSetName()));
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setConnectionStringReturnValue(kDonorConnStr);
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setFindHostReturnValue(kDonorConnStr.getServers()[0]);
        }

        _migrationId = MigrationSessionId::generate("donor", "recipient");

        SessionCatalog::create(getServiceContext());
        SessionCatalog::get(getServiceContext())->onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), stdx::make_unique<LogicalSessionCacheNoop>());
    }

    void tearDown() override {
        SessionCatalog::reset_forTest(getServiceContext());
        ShardingMongodTestFixture::tearDown();
    }

    void returnOplog(const std::vector<OplogEntry>& oplogList) {
        onCommand([&oplogList](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            BSONObjBuilder builder;
            BSONArrayBuilder arrBuilder(builder.subarrayStart("oplog"));

            for (const auto& oplog : oplogList) {
                arrBuilder.append(oplog.toBSON());
            }

            arrBuilder.doneFast();
            return builder.obj();
        });
    }

    repl::OplogEntry getOplog(OperationContext* opCtx, const repl::OpTime& opTime) {
        DBDirectClient client(opCtx);
        auto oplogBSON = client.findOne(NamespaceString::kRsOplogNamespace.ns(), opTime.asQuery());

        ASSERT_FALSE(oplogBSON.isEmpty());
        auto parseStatus = repl::OplogEntry::parse(oplogBSON);
        ASSERT_OK(parseStatus);

        return parseStatus.getValue();
    }

    MigrationSessionId migrationId() {
        return _migrationId.value();
    }

    ScopedSession getSessionWithTxn(OperationContext* opCtx,
                                    const LogicalSessionId& sessionId,
                                    const TxnNumber& txnNum) {
        auto scopedSession = SessionCatalog::get(opCtx)->getOrCreateSession(opCtx, sessionId);
        scopedSession->beginTxn(opCtx, txnNum);
        return scopedSession;
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

    void checkStatementExecuted(OperationContext* opCtx,
                                Session* session,
                                TxnNumber txnNumber,
                                StmtId stmtId) {
        auto oplog = session->checkStatementExecuted(opCtx, txnNumber, stmtId);
        ASSERT_TRUE(oplog);
    }

    void checkStatementExecuted(OperationContext* opCtx,
                                Session* session,
                                TxnNumber txnNumber,
                                StmtId stmtId,
                                repl::OplogEntry& expectedOplog) {
        auto oplog = session->checkStatementExecuted(opCtx, txnNumber, stmtId);
        ASSERT_TRUE(oplog);
        checkOplogWithNestedOplog(expectedOplog, *oplog);
    }

    void insertDocWithSessionInfo(const OperationSessionInfo& sessionInfo,
                                  const NamespaceString& ns,
                                  const BSONObj& doc,
                                  StmtId stmtId) {
        // Do write on a separate thread in order not to pollute this thread's opCtx.
        stdx::thread insertThread([sessionInfo, ns, doc, stmtId] {
            write_ops::WriteCommandBase cmdBase;
            std::vector<StmtId> stmtIds;
            stmtIds.push_back(stmtId);
            cmdBase.setStmtIds(stmtIds);

            write_ops::Insert insertRequest(ns);
            std::vector<BSONObj> documents;
            documents.push_back(doc);
            insertRequest.setDocuments(documents);
            insertRequest.setWriteCommandBase(cmdBase);

            BSONObjBuilder insertBuilder;
            insertRequest.serialize({}, &insertBuilder);
            sessionInfo.serialize(&insertBuilder);

            Client::initThread("test insert thread");
            auto innerOpCtx = Client::getCurrent()->makeOperationContext();

            // The ephemeral for test storage engine doesn't support document-level locking, so
            // requests with txnNumbers aren't allowed. To get around this, we have to manually set
            // up the session state and perform the insert.
            uassertStatusOK(initializeOperationSessionInfo(
                innerOpCtx.get(), insertBuilder.obj(), true, true, true));
            OperationContextSession sessionTxnState(innerOpCtx.get(), true);
            const auto reply = performInserts(innerOpCtx.get(), insertRequest);
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

private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() : ShardingCatalogClientMock(nullptr) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {

                ShardType donorShard;
                donorShard.setName(kDonorConnStr.getSetName());
                donorShard.setHost(kDonorConnStr.toString());

                return repl::OpTimeWith<std::vector<ShardType>>({donorShard});
            }
        };

        return stdx::make_unique<StaticCatalogClient>();
    }

    void _checkOplogExceptO2(const repl::OplogEntry& originalOplog,
                             const repl::OplogEntry& oplogToCheck) {
        ASSERT_TRUE(oplogToCheck.getOpType() == OpTypeEnum::kNoop);

        ASSERT_TRUE(oplogToCheck.getStatementId());
        ASSERT_EQ(*originalOplog.getStatementId(), *oplogToCheck.getStatementId());

        const auto origSessionInfo = originalOplog.getOperationSessionInfo();
        const auto sessionInfoToCheck = oplogToCheck.getOperationSessionInfo();

        ASSERT_TRUE(sessionInfoToCheck.getSessionId());
        ASSERT_EQ(*origSessionInfo.getSessionId(), *sessionInfoToCheck.getSessionId());

        ASSERT_TRUE(sessionInfoToCheck.getTxnNumber());
        ASSERT_EQ(*origSessionInfo.getTxnNumber(), *sessionInfoToCheck.getTxnNumber());

        ASSERT_BSONOBJ_EQ(kSessionOplogTag, oplogToCheck.getObject());
    }

    boost::optional<MigrationSessionId> _migrationId;
};

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyWhenNothingToTransfer) {
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

    sessionMigration.start(getServiceContext());
    finishSessionExpectSuccess(&sessionMigration);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithSameTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // sessionInfo
                                 Date_t::now(),                // wall clock time
                                 5);                           // statement id

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 2, 23, oplog1);
    checkStatementExecuted(opCtx, session.get(), 2, 45, oplog2);
    checkStatementExecuted(opCtx, session.get(), 2, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldOnlyStoreHistoryOfLatestTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    sessionInfo.setTxnNumber(txnNum++);
    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    sessionInfo.setTxnNumber(txnNum);
    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 5);                           // statement id

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, txnNum);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(txnNum));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));
    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), txnNum, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithSameTxnInSeparateBatches) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 5);                           // statement id

    // Return in 2 batches
    returnOplog({oplog1, oplog2});
    returnOplog({oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 2, 23, oplog1);
    checkStatementExecuted(opCtx, session.get(), 2, 45, oplog2);
    checkStatementExecuted(opCtx, session.get(), 2, 5, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithDifferentSession) {
    const auto sessionId1 = makeLogicalSessionIdForTest();
    const auto sessionId2 = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo2,                 // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo2,                 // session info
                                 Date_t::now(),                // wall clock time
                                 5);                           // statement id

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();

    {
        auto session = getSessionWithTxn(opCtx, sessionId1, 2);
        TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));
        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));
        ASSERT_FALSE(historyIter.hasNext());

        checkStatementExecuted(opCtx, session.get(), 2, 23, oplog1);
    }

    {
        auto session = getSessionWithTxn(opCtx, sessionId2, 42);
        TransactionHistoryIterator historyIter(session->getLastWriteOpTime(42));

        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

        ASSERT_TRUE(historyIter.hasNext());
        checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

        ASSERT_FALSE(historyIter.hasNext());

        checkStatementExecuted(opCtx, session.get(), 42, 45, oplog2);
        checkStatementExecuted(opCtx, session.get(), 42, 5, oplog3);
    }
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldNotNestAlreadyNestedOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
    sessionMigration.start(getServiceContext());

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(2);

    auto origInnerOplog1 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                          OpTypeEnum::kInsert,           // op type
                                          BSON("x" << 100),              // o
                                          boost::none,                   // o2
                                          sessionInfo,                   // session info
                                          boost::none,                   // wall clock time
                                          23);                           // statement id

    auto origInnerOplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                          OpTypeEnum::kInsert,          // op type
                                          BSON("x" << 80),              // o
                                          boost::none,                  // o2
                                          sessionInfo,                  // session info
                                          boost::none,                  // wall clock time
                                          45);                          // statement id

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(1100, 2), 1),  // optime
                                 OpTypeEnum::kNoop,              // op type
                                 BSONObj(),                      // o
                                 origInnerOplog1.toBSON(),       // o2
                                 sessionInfo,                    // session info
                                 Date_t::now(),                  // wall clock time
                                 23);                            // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(1080, 2), 1),  // optime
                                 OpTypeEnum::kNoop,              // op type
                                 BSONObj(),                      // o
                                 origInnerOplog2.toBSON(),       // o2
                                 sessionInfo,                    // session info
                                 Date_t::now(),                  // wall clock time
                                 45);                            // statement id

    returnOplog({oplog1, oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 2, 23);
    checkStatementExecuted(opCtx, session.get(), 2, 45);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandlePreImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                        45);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      45,                                  // statement id
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);                        // post-image optime

    returnOplog({preImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_TRUE(nextOplog.getStatementId());
    ASSERT_EQ(45, nextOplog.getStatementId().value());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(kSessionOplogTag, nextOplog.getObject());

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

    ASSERT_TRUE(newPreImageOplog.getStatementId());
    ASSERT_EQ(45, newPreImageOplog.getStatementId().value());

    auto preImageSessionInfo = newPreImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(preImageOplog.getObject(), newPreImageOplog.getObject());
    ASSERT_TRUE(newPreImageOplog.getObject2());
    ASSERT_TRUE(newPreImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, session.get(), 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandlePostImageFindAndModify) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                         45);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                      OpTypeEnum::kUpdate,          // op type
                                      BSON("x" << 100),             // o
                                      BSON("$set" << BSON("x" << 101)),
                                      sessionInfo,                          // session info
                                      Date_t::now(),                        // wall clock time
                                      45,                                   // statement id
                                      boost::none,                          // pre-image optime
                                      repl::OpTime(Timestamp(100, 2), 1));  // post-image optime

    returnOplog({postImageOplog, updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_TRUE(nextOplog.getStatementId());
    ASSERT_EQ(45, nextOplog.getStatementId().value());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(kSessionOplogTag, nextOplog.getObject());

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

    ASSERT_TRUE(newPostImageOplog.getStatementId());
    ASSERT_EQ(45, newPostImageOplog.getStatementId().value());

    auto preImageSessionInfo = newPostImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(postImageOplog.getObject(), newPostImageOplog.getObject());
    ASSERT_TRUE(newPostImageOplog.getObject2());
    ASSERT_TRUE(newPostImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, session.get(), 2, 45, updateOplog);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldBeAbleToHandleFindAndModifySplitIn2Batches) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                        45);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      45,                                  // statement id
                                      repl::OpTime(Timestamp(100, 2), 1),  // pre-image optime
                                      boost::none);

    returnOplog({preImageOplog});
    returnOplog({updateOplog});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());

    auto nextOplog = historyIter.next(opCtx);
    ASSERT_TRUE(nextOplog.getOpType() == OpTypeEnum::kNoop);

    ASSERT_TRUE(nextOplog.getStatementId());
    ASSERT_EQ(45, nextOplog.getStatementId().value());

    auto nextSessionInfo = nextOplog.getOperationSessionInfo();

    ASSERT_TRUE(nextSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, nextSessionInfo.getSessionId().value());

    ASSERT_TRUE(nextSessionInfo.getTxnNumber());
    ASSERT_EQ(2, nextSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(kSessionOplogTag, nextOplog.getObject());

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

    ASSERT_TRUE(newPreImageOplog.getStatementId());
    ASSERT_EQ(45, newPreImageOplog.getStatementId().value());

    auto preImageSessionInfo = newPreImageOplog.getOperationSessionInfo();

    ASSERT_TRUE(preImageSessionInfo.getSessionId());
    ASSERT_EQ(sessionId, preImageSessionInfo.getSessionId().value());

    ASSERT_TRUE(preImageSessionInfo.getTxnNumber());
    ASSERT_EQ(2, preImageSessionInfo.getTxnNumber().value());

    ASSERT_BSONOBJ_EQ(preImageOplog.getObject(), newPreImageOplog.getObject());
    ASSERT_TRUE(newPreImageOplog.getObject2());
    ASSERT_TRUE(newPreImageOplog.getObject2().value().isEmpty());

    checkStatementExecuted(opCtx, session.get(), 2, 45, updateOplog);
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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 oldSessionInfo,               // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    returnOplog({oplog1, oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto session = getSessionWithTxn(opCtx, sessionId, 20);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(20));

    ASSERT_TRUE(historyIter.hasNext());
    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 20, 0);
}

TEST_F(SessionCatalogMigrationDestinationTest, NewerTxnWriteShouldNotBeOverwrittenByOldMigrateTxn) {
    const auto sessionId = makeLogicalSessionIdForTest();

    auto opCtx = operationContext();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);            // statement id

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
                                 45);                          // statement id

    returnOplog({oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    auto session = getSessionWithTxn(opCtx, sessionId, 20);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(20));

    ASSERT_TRUE(historyIter.hasNext());
    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    checkStatementExecuted(opCtx, session.get(), 20, 0);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyAfterNetworkError) {
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                23);                           // statement id

    returnOplog({oplog});

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoTxnNumber) {
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                23);                           // statement id

    returnOplog({oplog});

    sessionMigration.join();
    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldJoinProperlyForResponseWithNoStmtId) {
    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                boost::none);                  // statement id

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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

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
                                 45);                          // statement id

    returnOplog({oplog2});

    finishSessionExpectSuccess(&sessionMigration);

    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog2, historyIter.next(opCtx));

    auto oplog = historyIter.next(opCtx);
    ASSERT_BSONOBJ_EQ(BSON("_id"
                           << "newerSess"),
                      oplog.getObject());

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 2, 0);
    checkStatementExecuted(opCtx, session.get(), 2, 23, oplog1);
    checkStatementExecuted(opCtx, session.get(), 2, 45, oplog2);
}

TEST_F(SessionCatalogMigrationDestinationTest, ShouldErrorForConsecutivePreImageOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                         45);                           // statement id

    auto preImageOplog2 = makeOplogEntry(OpTime(Timestamp(100, 2), 1),  // optime
                                         OpTypeEnum::kNoop,             // op type
                                         BSON("x" << 100),              // o
                                         boost::none,                   // o2
                                         sessionInfo,                   // session info
                                         Date_t::now(),                 // wall clock time
                                         45);                           // statement id

    returnOplog({preImageOplog1, preImageOplog2});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorForPreImageOplogWithNonMatchingSessionId) {

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                        45);                           // statement id

    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                      OpTypeEnum::kUpdate,          // op type
                                      BSON("x" << 100),             // o
                                      BSON("$set" << BSON("x" << 101)),
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      45,                                  // statement id
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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                        45);                           // statement id

    sessionInfo.setTxnNumber(56);
    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      45,                                  // statement id
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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                        45);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),       // optime
                                      OpTypeEnum::kUpdate,               // op type
                                      BSON("x" << 100),                  // o
                                      BSON("$set" << BSON("x" << 101)),  // o2
                                      sessionInfo,                       // session info
                                      Date_t::now(),                     // wall clock time
                                      45);                               // statement id

    returnOplog({preImageOplog, updateOplog});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::ErrorOccurred ==
                sessionMigration.getState());
    ASSERT_FALSE(sessionMigration.getErrMsg().empty());
}

TEST_F(SessionCatalogMigrationDestinationTest,
       ShouldErrorIfOplogWithPreImageLinkIsPrecededByNormalOplog) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                 23);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),         // optime
                                      OpTypeEnum::kUpdate,                 // op type
                                      BSON("x" << 100),                    // o
                                      BSON("$set" << BSON("x" << 101)),    // o2
                                      sessionInfo,                         // session info
                                      Date_t::now(),                       // wall clock time
                                      45,                                  // statement id
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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());

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
                                 23);                           // statement id

    auto updateOplog = makeOplogEntry(OpTime(Timestamp(80, 2), 1),          // optime
                                      OpTypeEnum::kUpdate,                  // op type
                                      BSON("x" << 100),                     // o
                                      BSON("$set" << BSON("x" << 101)),     // o2
                                      sessionInfo,                          // session info
                                      Date_t::now(),                        // wall clock time
                                      45,                                   // statement id
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

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
    sessionMigration.start(getServiceContext());

    auto oplog1 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 100),             // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 23);                          // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(70, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 30);                          // statement id

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 80),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 45);                          // statement id

    returnOplog({oplog1, oplog2, oplog3});

    finishSessionExpectSuccess(&sessionMigration);

    auto session = getSessionWithTxn(opCtx, sessionId, 19);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(19));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    auto firstInsertOplog = historyIter.next(opCtx);

    ASSERT_TRUE(firstInsertOplog.getOpType() == OpTypeEnum::kInsert);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 46), firstInsertOplog.getObject());
    ASSERT_TRUE(firstInsertOplog.getStatementId());
    ASSERT_EQ(30, *firstInsertOplog.getStatementId());

    checkStatementExecuted(opCtx, session.get(), 19, 23, oplog1);
    checkStatementExecuted(opCtx, session.get(), 19, 30);
    checkStatementExecuted(opCtx, session.get(), 19, 45, oplog3);
}

TEST_F(SessionCatalogMigrationDestinationTest, OplogEntriesWithIncompleteHistory) {
    const auto sessionId = makeLogicalSessionIdForTest();

    SessionCatalogMigrationDestination sessionMigration(kFromShard, migrationId());
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
                                 23);                           // statement id

    auto oplog2 = makeOplogEntry(OpTime(Timestamp(80, 2), 1),  // optime
                                 OpTypeEnum::kNoop,            // op type
                                 {},                           // o
                                 Session::kDeadEndSentinel,    // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 kIncompleteHistoryStmtId);    // statement id

    auto oplog3 = makeOplogEntry(OpTime(Timestamp(60, 2), 1),  // optime
                                 OpTypeEnum::kInsert,          // op type
                                 BSON("x" << 60),              // o
                                 boost::none,                  // o2
                                 sessionInfo,                  // session info
                                 Date_t::now(),                // wall clock time
                                 5);                           // statement id

    returnOplog({oplog1, oplog2, oplog3});
    // migration always fetches at least twice to transition from committing to done.
    returnOplog({});
    returnOplog({});

    sessionMigration.join();

    ASSERT_TRUE(SessionCatalogMigrationDestination::State::Done == sessionMigration.getState());

    auto opCtx = operationContext();
    auto session = getSessionWithTxn(opCtx, sessionId, 2);
    TransactionHistoryIterator historyIter(session->getLastWriteOpTime(2));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog3, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplog(oplog2, historyIter.next(opCtx));

    ASSERT_TRUE(historyIter.hasNext());
    checkOplogWithNestedOplog(oplog1, historyIter.next(opCtx));

    ASSERT_FALSE(historyIter.hasNext());

    checkStatementExecuted(opCtx, session.get(), 2, 23, oplog1);
    checkStatementExecuted(opCtx, session.get(), 2, 5, oplog3);
    ASSERT_THROWS(session->checkStatementExecuted(opCtx, 2, 38), AssertionException);
}

}  // namespace

}  // namespace mongo
