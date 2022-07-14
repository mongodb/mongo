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

#include <map>
#include <set>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/txn_retry_counter_too_old_info.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/router_transactions_metrics.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const BSONObj kOkReadOnlyFalseResponse = BSON("ok" << 1 << "readOnly" << false);
const BSONObj kOkReadOnlyTrueResponse = BSON("ok" << 1 << "readOnly" << true);
const BSONObj kNoSuchTransactionResponse =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction);
const BSONObj kDummyFindCmd = BSON("find"
                                   << "dummy");

class TransactionRouterTest : public ShardingTestFixture {
protected:
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

    const ShardId shard1 = ShardId("shard1");
    const HostAndPort hostAndPort1 = HostAndPort("shard1:1234");

    const ShardId shard2 = ShardId("shard2");
    const HostAndPort hostAndPort2 = HostAndPort("shard2:1234");

    const ShardId shard3 = ShardId("shard3");
    const HostAndPort hostAndPort3 = HostAndPort("shard3:1234");

    const StringMap<repl::ReadConcernLevel> supportedNonSnapshotRCLevels = {
        {"local", repl::ReadConcernLevel::kLocalReadConcern},
        {"majority", repl::ReadConcernLevel::kMajorityReadConcern}};

    const std::vector<repl::ReadConcernLevel> unsupportedRCLevels = {
        repl::ReadConcernLevel::kAvailableReadConcern,
        repl::ReadConcernLevel::kLinearizableReadConcern};

    const Status kDummyStatus = {ErrorCodes::InternalError, "dummy"};

    const Status kDummyRetryableStatus = {ErrorCodes::InterruptedDueToReplStateChange, "dummy"};

    const BSONObj kDummyOkRes = BSON("ok" << 1);

    const BSONObj kDummyErrorRes = BSON("ok" << 0 << "code" << kDummyStatus.code());

    const BSONObj kDummyRetryableErrorRes =
        BSON("ok" << 0 << "code" << kDummyRetryableStatus.code());

    const BSONObj kDummyWriteConcernError =
        BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                    << "dummy");

    const BSONObj kDummyResWithWriteConcernError =
        BSON("ok" << 1 << "writeConcernError" << kDummyWriteConcernError);

    const NamespaceString kViewNss = NamespaceString("test.foo");

    const Status kStaleConfigStatus = {
        StaleConfigInfo(kViewNss, ChunkVersion::UNSHARDED(), boost::none, shard1),
        "The metadata for the collection is not loaded"};

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        addRemoteShards({std::make_tuple(shard1, hostAndPort1),
                         std::make_tuple(shard2, hostAndPort2),
                         std::make_tuple(shard3, hostAndPort3)});

        APIParameters::get(operationContext()) = APIParameters();
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Set the initial clusterTime.
        VectorClock::get(getServiceContext())->advanceClusterTime_forTest(kInMemoryLogicalTime);

        // Set up a tick source for transaction metrics.
        auto tickSource = std::make_unique<TickSourceMock<Microseconds>>();
        tickSource->reset(1);
        getServiceContext()->setTickSource(std::move(tickSource));

        _staleVersionAndSnapshotRetriesBlock = std::make_unique<FailPointEnableBlock>(
            "enableStaleVersionAndSnapshotRetriesWithinTransactions");
    }

    void disableRouterRetriesFailPoint() {
        _staleVersionAndSnapshotRetriesBlock.reset();
    }

    /**
     * Verifies "abortTransaction" is sent to each expected HostAndPort with the given lsid and
     * txnNumber. The aborts may come in any order.
     */
    void expectAbortTransactions(std::set<HostAndPort> expectedHostAndPorts,
                                 LogicalSessionId lsid,
                                 TxnNumber txnNum,
                                 BSONObj abortResponse = kOkReadOnlyFalseResponse) {
        std::set<HostAndPort> seenHostAndPorts;
        int numExpectedAborts = static_cast<int>(expectedHostAndPorts.size());
        for (int i = 0; i < numExpectedAborts; i++) {
            onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
                seenHostAndPorts.insert(request.target);

                ASSERT_EQ(NamespaceString::kAdminDb, request.dbname);

                auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
                ASSERT_EQ(cmdName, "abortTransaction");

                auto osi = OperationSessionInfoFromClient::parse("expectAbortTransaction"_sd,
                                                                 request.cmdObj);

                ASSERT(osi.getSessionId());
                ASSERT_EQ(lsid.getId(), osi.getSessionId()->getId());

                ASSERT(osi.getTxnNumber());
                ASSERT_EQ(txnNum, *osi.getTxnNumber());

                ASSERT(osi.getAutocommit());
                ASSERT_FALSE(*osi.getAutocommit());

                return abortResponse;
            });
        }

        ASSERT(expectedHostAndPorts == seenHostAndPorts);
    }

    void expectCommitTransaction(StatusWith<BSONObj> swRes = StatusWith<BSONObj>(BSON("ok" << 1))) {
        onCommand([&](const RemoteCommandRequest& request) {
            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "commitTransaction");
            return swRes;
        });
    }

    void expectCoordinateCommitTransaction(
        StatusWith<BSONObj> swRes = StatusWith<BSONObj>(BSON("ok" << 1))) {
        onCommand([&](const RemoteCommandRequest& request) {
            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "coordinateCommitTransaction");
            return swRes;
        });
    }

    void runFunctionFromDifferentOpCtx(std::function<void(OperationContext*)> func) {
        auto newClientOwned = getServiceContext()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();
        func(newOpCtx.get());
    }

    void runTransactionLeaveOpen(LogicalSessionId lsid, TxnNumber txnNumber) {
        runFunctionFromDifferentOpCtx([&](OperationContext* opCtx) {
            opCtx->setLogicalSessionId(lsid);
            opCtx->setTxnNumber(txnNumber);
            opCtx->setInMultiDocumentTransaction();
            auto opCtxSession = std::make_unique<RouterOperationContextSession>(opCtx);

            auto txnRouter = TransactionRouter::get(opCtx);
            txnRouter.beginOrContinueTxn(
                opCtx, *opCtx->getTxnNumber(), TransactionRouter::TransactionActions::kStart);
        });
    }

private:
    // Enables the transaction router to retry within a transaction on stale version and snapshot
    // errors for the duration of each test.
    // TODO SERVER-39704: Remove this failpoint block.
    std::unique_ptr<FailPointEnableBlock> _staleVersionAndSnapshotRetriesBlock;
};

class TransactionRouterTestWithDefaultSession : public TransactionRouterTest {
protected:
    void setUp() override {
        TransactionRouterTest::setUp();

        const auto opCtx = operationContext();
        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());

        _routerOpCtxSession.emplace(opCtx);
    }

    void tearDown() override {
        _routerOpCtxSession.reset();

        TransactionRouterTest::tearDown();
    }

    const LogicalSessionId& getSessionId() {
        return *operationContext()->getLogicalSessionId();
    }

private:
    boost::optional<RouterOperationContextSession> _routerOpCtxSession;
};

TEST_F(TransactionRouterTestWithDefaultSession,
       StartTxnShouldBeAttachedOnlyOnFirstStatementToParticipant) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false << "txnNumber" << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator" << true << "autocommit" << false << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, BasicStartTxnWithAtClusterTime) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false << "txnNumber" << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator" << true << "autocommit" << false << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotContiueTxnWithoutStarting) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTestWithDefaultSession, NewParticipantMustAttachTxnAndReadConcern) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false << "txnNumber" << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator" << true << "autocommit" << false << "txnNumber"
                               << txnNum),
                          newCmd);
    }

    expectedNewObj = BSON("insert"
                          << "test"
                          << "readConcern"
                          << BSON("level"
                                  << "snapshot"
                                  << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                          << "startTransaction" << true << "autocommit" << false << "txnNumber"
                          << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard2,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard2,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit" << false << "txnNumber" << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, StartingNewTxnShouldClearState) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "readConcern"
                               << BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                               << "startTransaction" << true << "coordinator" << true
                               << "autocommit" << false << "txnNumber" << txnNum),
                          newCmd);
    }

    TxnNumber txnNum2{5};
    operationContext()->setTxnNumber(txnNum2);
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false << "txnNumber" << txnNum2);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       DoNotAttachTxnRetryCounterIfTxnRetryCounterIsDefault) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("insert"
                                                         << "test"
                                                         << "txnNumber" << txnNum));
    ASSERT_EQ(newCmd.hasField("txnRetryCounter"), false);
}

TEST_F(TransactionRouterTestWithDefaultSession, FirstParticipantIsCoordinator) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        auto& participant = *txnRouter.getParticipant(shard1);
        ASSERT(participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    {
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        auto& participant = *txnRouter.getParticipant(shard2);
        ASSERT(!participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    TxnNumber txnNum2{5};
    operationContext()->setTxnNumber(txnNum2);
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        auto& participant = *txnRouter.getParticipant(shard2);
        ASSERT(participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, RecoveryShardDoesNotGetSetForReadOnlyTransaction) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // The recovery shard is unset initially.
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // The recovery shard is not set on scheduling requests.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // The recovery shard is not set if a participant responds with ok but that it is read-only.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // The recovery shard is not set even if more read-only participants respond.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyTrueResponse);
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    // The recovery shard is not set even if the participants say they did a write for commit.
    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), boost::none); });
    for (int i = 0; i < 2; i++) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ("admin", request.dbname);
            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "commitTransaction");
            return kOkReadOnlyFalseResponse;
        });
    }
    ASSERT_FALSE(txnRouter.getRecoveryShardId());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RecoveryShardIsSetToSingleParticipantIfSingleParticipantDoesWriteOnFirstStatement) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RecoveryShardIsSetToSingleParticipantIfSingleParticipantDoesWriteOnLaterStatement) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // Response to first statement says read-only.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // Response to second statement says not read-only.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RecoveryShardIsSetToSecondParticipantIfSecondParticipantIsFirstToDoAWrite) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Shard1's response says read-only.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // Shard2's response says not read-only.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard2);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RecoveryShardIsResetIfRecoveryParticipantIsPendingAndPendingParticipantsAreCleared) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Shard1's response says not read-only.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);

    // Participant list is cleared.
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.default_timed_get();

    ASSERT_FALSE(txnRouter.getRecoveryShardId());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RecoveryShardIsNotResetIfRecoveryParticipantIsNotPendingAndPendingParticipantsAreCleared) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Shard1's response says not read-only.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);

    // New statement.
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    // Shard2 responds, it doesn't matter whether it's read-only, just that it's a pending
    // participant.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);

    // Participant list is cleared.
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort2}, getSessionId(), txnNum);
    future.default_timed_get();

    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);
}

TEST_F(TransactionRouterTestWithDefaultSession, RecoveryShardIsResetOnStartingNewTransaction) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Shard1's response says not read-only.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shard1);

    // Start new transaction on session.
    TxnNumber newTxnNum{4};
    txnRouter.beginOrContinueTxn(
        operationContext(), newTxnNum, TransactionRouter::TransactionActions::kStart);

    ASSERT_FALSE(txnRouter.getRecoveryShardId());
}

TEST_F(TransactionRouterTestWithDefaultSession, DoesNotAttachTxnNumIfAlreadyThere) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "txnNumber" << txnNum << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("insert"
                                                         << "test"
                                                         << "txnNumber" << txnNum));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             CrashesIfCmdHasDifferentTxnNumber,
             "invariant") {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                      shard1,
                                      BSON("insert"
                                           << "test"
                                           << "txnNumber" << TxnNumber(10)));
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             CannotUnstashWithDifferentTxnNumber,
             "The requested operation has a different transaction number") {
    TxnNumber txnNum{3};

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    operationContext()->setTxnNumber(txnNum + 1);
    txnRouter.stash(operationContext(), TransactionRouter::StashReason::kYield);
    txnRouter.unstash(operationContext());
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             CannotUnstashWithNoTxnNumber,
             "Cannot unstash without a transaction number") {
    TxnNumber txnNum{3};

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.stash(operationContext(), TransactionRouter::StashReason::kYield);
    txnRouter.unstash(operationContext());
}

TEST_F(TransactionRouterTestWithDefaultSession, SuccessfullyUnstashWithMatchingTxnNumber) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    operationContext()->setTxnNumber(txnNum);
    txnRouter.stash(operationContext(), TransactionRouter::StashReason::kYield);
    txnRouter.unstash(operationContext());
}

TEST_F(TransactionRouterTestWithDefaultSession, AttachTxnValidatesReadConcernIfAlreadyOnCmd) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"
                                                             << "readConcern"
                                                             << BSON("level"
                                                                     << "snapshot")));
        ASSERT_BSONOBJ_EQ(BSON("insert"
                               << "test"
                               << "readConcern"
                               << BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp())
                               << "startTransaction" << true << "coordinator" << true
                               << "autocommit" << false << "txnNumber" << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, SameAPIParametersAfterFirstStatement) {
    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("1");
    APIParameters::get(operationContext()) = apiParameters;
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Continuing with the same API params succeeds. (Must reset readConcern from "snapshot".)
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);
}

TEST_F(TransactionRouterTestWithDefaultSession, DifferentAPIParametersAfterFirstStatement) {
    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("1");
    APIParameters::get(operationContext()) = apiParameters;
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Can't continue with different params. (Must reset readConcern from "snapshot".)
    APIParameters::get(operationContext()).setAPIStrict(true);
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        DBException,
        ErrorCodes::APIMismatchError);
}

TEST_F(TransactionRouterTestWithDefaultSession, NoAPIParametersAfterFirstStatement) {
    APIParameters apiParameters = APIParameters();
    apiParameters.setAPIVersion("1");
    APIParameters::get(operationContext()) = apiParameters;
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Can't continue without params. (Must reset readConcern from "snapshot".)
    APIParameters::get(operationContext()) = APIParameters();
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        DBException,
        ErrorCodes::APIMismatchError);
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotSpecifyReadConcernAfterFirstStatement) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(TransactionRouterTestWithDefaultSession, PassesThroughEmptyReadConcernToParticipants) {
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern" << BSONObj() << "startTransaction" << true
                                  << "coordinator" << true << "autocommit" << false << "txnNumber"
                                  << txnNum);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("insert"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       PassesThroughNoReadConcernLevelToParticipantsWithAfterClusterTime) {
    LogicalTime kAfterClusterTime(Timestamp(10, 1));
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(kAfterClusterTime, boost::none);

    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("afterClusterTime" << kAfterClusterTime.asTimestamp())
                                  << "startTransaction" << true << "coordinator" << true
                                  << "autocommit" << false << "txnNumber" << txnNum);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("insert"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTestWithDefaultSession, RejectUnsupportedReadConcernLevels) {
    for (auto readConcernLevel : unsupportedRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(readConcernLevel);

        TxnNumber txnNum{3};
        operationContext()->setTxnNumber(txnNum);
        auto txnRouter = TransactionRouter::get(operationContext());
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(
                operationContext(), txnNum, TransactionRouter::TransactionActions::kStart),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, RejectUnsupportedLevelsWithAfterClusterTime) {
    for (auto readConcernLevel : unsupportedRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(LogicalTime(Timestamp(10, 1)), readConcernLevel);

        TxnNumber txnNum{3};
        operationContext()->setTxnNumber(txnNum);
        auto txnRouter = TransactionRouter::get(operationContext());
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(
                operationContext(), txnNum, TransactionRouter::TransactionActions::kStart),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, RejectUnsupportedLevelsWithAfterOpTime) {
    for (auto readConcernLevel : unsupportedRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::OpTime(Timestamp(10, 1), 2), readConcernLevel);

        TxnNumber txnNum{3};
        operationContext()->setTxnNumber(txnNum);
        auto txnRouter = TransactionRouter::get(operationContext());
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(
                operationContext(), txnNum, TransactionRouter::TransactionActions::kStart),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotCommitWithoutParticipantsOrRecoveryToken) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_THROWS(txnRouter.commitTransaction(operationContext(), boost::none), AssertionException);
}

void checkSessionDetails(const BSONObj& cmdObj,
                         const LogicalSessionId& lsid,
                         const TxnNumber txnNum,
                         boost::optional<bool> isCoordinator) {
    auto osi = OperationSessionInfoFromClient::parse("testTxnRouter"_sd, cmdObj);

    ASSERT(osi.getSessionId());
    ASSERT_EQ(lsid.getId(), osi.getSessionId()->getId());

    ASSERT(osi.getTxnNumber());
    ASSERT_EQ(txnNum, *osi.getTxnNumber());

    ASSERT(osi.getAutocommit());
    ASSERT_FALSE(*osi.getAutocommit());

    ASSERT_TRUE(cmdObj["txnRetryCounter"].eoo());

    if (isCoordinator) {
        ASSERT_EQ(*isCoordinator, cmdObj["coordinator"].trueValue());
    } else {
        ASSERT_TRUE(cmdObj["coordinator"].eoo());
    }
}

void checkWriteConcern(const BSONObj& cmdObj, const WriteConcernOptions& expectedWC) {
    auto writeConcernElem = cmdObj["writeConcern"];
    ASSERT_FALSE(writeConcernElem.eoo());
    ASSERT_BSONOBJ_EQ(expectedWC.toBSON(), writeConcernElem.Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       CommitTransactionWithNoParticipantsDoesNotSendCommit) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    auto commitResult = txnRouter.commitTransaction(operationContext(), boost::none);
    ASSERT_BSONOBJ_EQ(commitResult, BSON("ok" << 1));

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForSingleParticipantThatIsReadOnly) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "commitTransaction");

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForSingleParticipantThatDidAWrite) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "commitTransaction");

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForMultipleParticipantsThatAreAllReadOnly) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyTrueResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    // The requests are scheduled in a nondeterministic order, since they are scheduled by iterating
    // over the participant list, which is stored as a hash map. So, just check that all expected
    // hosts and ports were targeted at the end.
    std::set<HostAndPort> expectedHostAndPorts{hostAndPort1, hostAndPort2};
    std::set<HostAndPort> seenHostAndPorts;
    for (int i = 0; i < 2; i++) {
        onCommand([&](const RemoteCommandRequest& request) {
            seenHostAndPorts.insert(request.target);

            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "commitTransaction");

            // The shard with hostAndPort1 is expected to be the coordinator.
            checkSessionDetails(
                request.cmdObj, getSessionId(), txnNum, (request.target == hostAndPort1));

            return kOkReadOnlyTrueResponse;
        });
    }

    future.default_timed_get();
    ASSERT(expectedHostAndPorts == seenHostAndPorts);
}

// TODO (SERVER-48340): Re-enable the single-write-shard transaction commit optimization.
TEST_F(TransactionRouterTestWithDefaultSession,
       SendCoordinateCommitForMultipleParticipantsOnlyOneDidAWrite) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        std::set<std::string> expectedParticipants = {shard1.toString(), shard2.toString()};
        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_EQ(expectedParticipants.size(), participantElements.size());

        for (const auto& element : participantElements) {
            auto shardId = element["shardId"].str();
            ASSERT_EQ(1ull, expectedParticipants.count(shardId));
            expectedParticipants.erase(shardId);
        }

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCoordinateCommitForMultipleParticipantsMoreThanOneDidAWrite) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        std::set<std::string> expectedParticipants = {shard1.toString(), shard2.toString()};
        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_EQ(expectedParticipants.size(), participantElements.size());

        for (const auto& element : participantElements) {
            auto shardId = element["shardId"].str();
            ASSERT_EQ(1ull, expectedParticipants.count(shardId));
            expectedParticipants.erase(shardId);
        }

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTest, CommitWithRecoveryTokenWithNoParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_TRUE(participantElements.empty());

        checkSessionDetails(request.cmdObj, lsid, txnNum, true /* isCoordinator */);
        checkWriteConcern(request.cmdObj, writeConcern);

        return BSON("ok" << 1);
    });

    future.default_timed_get();

    // Sending commit with a recovery token again should cause the router to use the recovery path
    // again.

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    future = launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_TRUE(participantElements.empty());

        checkSessionDetails(request.cmdObj, lsid, txnNum, true /* isCoordinator */);
        checkWriteConcern(request.cmdObj, writeConcern);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       CrossShardTxnCommitWorksAfterRecoveryCommitForPreviousTransaction) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto opCtx = operationContext();
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    auto txnRouter = TransactionRouter::get(opCtx);
    // Simulate recovering a commit with a recovery token and no participants.
    {
        txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

        TxnRecoveryToken recoveryToken;
        recoveryToken.setRecoveryShardId(shard1);

        auto future =
            launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(hostAndPort1, request.target);
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "coordinateCommitTransaction");

            auto participantElements = request.cmdObj["participants"].Array();
            ASSERT_TRUE(participantElements.empty());

            checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);
            checkWriteConcern(request.cmdObj, writeConcern);

            return BSON("ok" << 1);
        });

        future.default_timed_get();
    }

    // Increase the txn number and run a cross-shard transaction with two-phase commit. The commit
    // should be sent with the correct participant list.
    {
        ++txnNum;
        operationContext()->setTxnNumber(txnNum);
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
        txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);

        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

        TxnRecoveryToken recoveryToken;
        recoveryToken.setRecoveryShardId(shard1);

        auto future =
            launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(hostAndPort1, request.target);
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "coordinateCommitTransaction");

            std::set<std::string> expectedParticipants = {shard1.toString(), shard2.toString()};
            auto participantElements = request.cmdObj["participants"].Array();
            ASSERT_EQ(expectedParticipants.size(), participantElements.size());

            for (const auto& element : participantElements) {
                auto shardId = element["shardId"].str();
                ASSERT_EQ(1ull, expectedParticipants.count(shardId));
                expectedParticipants.erase(shardId);
            }

            checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

            return BSON("ok" << 1);
        });

        future.default_timed_get();
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RouterShouldWorkAsRecoveryRouterEvenIfItHasSeenPreviousTransactions) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto opCtx = operationContext();
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    auto txnRouter = TransactionRouter::get(opCtx);
    // Run a cross-shard transaction with two-phase commit. The commit should be sent with the
    // correct participant list.
    {
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
        txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);

        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

        TxnRecoveryToken recoveryToken;
        recoveryToken.setRecoveryShardId(shard1);

        auto future =
            launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(hostAndPort1, request.target);
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "coordinateCommitTransaction");

            std::set<std::string> expectedParticipants = {shard1.toString(), shard2.toString()};
            auto participantElements = request.cmdObj["participants"].Array();
            ASSERT_EQ(expectedParticipants.size(), participantElements.size());

            for (const auto& element : participantElements) {
                auto shardId = element["shardId"].str();
                ASSERT_EQ(1ull, expectedParticipants.count(shardId));
                expectedParticipants.erase(shardId);
            }

            checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);

            return BSON("ok" << 1);
        });

        future.default_timed_get();
    }

    // Increase the txn number and simulate recovering a commit with a recovery token and no
    // participants.
    {
        ++txnNum;
        operationContext()->setTxnNumber(txnNum);
        txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

        TxnRecoveryToken recoveryToken;
        recoveryToken.setRecoveryShardId(shard1);

        auto future =
            launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(hostAndPort1, request.target);
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "coordinateCommitTransaction");

            auto participantElements = request.cmdObj["participants"].Array();
            ASSERT_TRUE(participantElements.empty());

            checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true /* isCoordinator */);
            checkWriteConcern(request.cmdObj, writeConcern);

            return BSON("ok" << 1);
        });

        future.default_timed_get();
    }
}

TEST_F(TransactionRouterTest, CommitWithEmptyRecoveryToken) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    ASSERT_THROWS_CODE(txnRouter.commitTransaction(operationContext(), recoveryToken),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, CommitWithRecoveryTokenWithUnknownShard) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setRecoveryShardId(ShardId("magicShard"));

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    ShardType shardType;
    shardType.setName(shard1.toString());
    shardType.setHost(hostAndPort1.toString());

    // ShardRegistry will try to perform a reload since it doesn't know about the shard.
    expectGetShards({shardType});

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::ShardNotFound);
}

TEST_F(TransactionRouterTestWithDefaultSession, SnapshotErrorsResetAtClusterTime) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    // Advance the latest time in the logical clock so the retry attempt will pick a later time.
    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    VectorClock::get(operationContext())->advanceClusterTime_forTest(laterTime);

    // Simulate a snapshot error.

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.default_timed_get();

    txnRouter.setDefaultAtClusterTime(operationContext());

    expectedReadConcern = BSON("level"
                               << "snapshot"
                               << "atClusterTime" << laterTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       CannotChangeAtClusterTimeAfterStatementThatSelectedIt) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    // Changing the atClusterTime during the statement that selected it is allowed.

    LogicalTime laterTimeSameStmt(Timestamp(100, 1));
    ASSERT_GT(laterTimeSameStmt, kInMemoryLogicalTime);
    VectorClock::get(operationContext())->advanceClusterTime_forTest(laterTimeSameStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    expectedReadConcern = BSON("level"
                               << "snapshot"
                               << "atClusterTime" << laterTimeSameStmt.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard2,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    // Later statements cannot change atClusterTime.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    LogicalTime laterTimeNewStmt(Timestamp(1000, 1));
    ASSERT_GT(laterTimeNewStmt, laterTimeSameStmt);
    VectorClock::get(operationContext())->advanceClusterTime_forTest(laterTimeNewStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard3,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, SnapshotErrorsClearsAllParticipants) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Successfully start a transaction on two shards, selecting one as the coordinator.

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate a snapshot error and an internal retry that only re-targets one of the original two
    // shards.

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1, hostAndPort2}, getSessionId(), txnNum);
    future.default_timed_get();

    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());

        newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        ASSERT_FALSE(newCmd["startTransaction"].trueValue());
    }

    // There is a new coordinator.
    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);

    {
        // Shard1 should also attach startTransaction field again.
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());

        newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        ASSERT_FALSE(newCmd["startTransaction"].trueValue());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotContinueOnSnapshotErrorAfterFirstCommand) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    ASSERT(txnRouter.canContinueOnSnapshotError());

    txnRouter.setDefaultAtClusterTime(operationContext());

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());
}

TEST_F(TransactionRouterTestWithDefaultSession, ParticipantsRememberStmtIdCreatedAt) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Transaction 1 contacts shard1 and shard2 during the first command, then shard3 in the second
    // command.

    int initialStmtId = 0;
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    ShardId shard3("shard3");
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd);
    ASSERT_EQ(txnRouter.getParticipant(shard3)->stmtIdCreatedAt, initialStmtId + 1);

    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    // Transaction 2 contacts shard3 and shard2 during the first command, then shard1 in the second
    // command.

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    TxnNumber txnNum2{5};
    operationContext()->setTxnNumber(txnNum2);
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    ASSERT_EQ(txnRouter.getParticipant(shard3)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kContinue);

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId + 1);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       LatestStmtIdNotIncrementedIfNoParticipantsTargeted) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    int initialStmtId = 0;
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    ASSERT_EQ(txnRouter.getLatestStmtId(), initialStmtId);

    // Ensure that latestStmtId gets incremented when a participant is targeted.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    ASSERT_EQ(txnRouter.getLatestStmtId(), 1);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AllParticipantsAndCoordinatorClearedOnStaleErrorOnFirstCommand) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Start a transaction on two shards, selecting one as the coordinator, but simulate a
    // re-targeting error from at least one of them.

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate stale error and internal retry that only re-targets one of the original shards.

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find", kDummyStatus));
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort1, hostAndPort2}, getSessionId(), txnNum);
    future.default_timed_get();

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        ASSERT_FALSE(txnRouter.getParticipant(shard2));
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());
    }

    // There is a new coordinator.
    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);

    {
        // Shard1 has not started a transaction.
        ASSERT_FALSE(txnRouter.getParticipant(shard1));
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, OnlyNewlyCreatedParticipantsClearedOnStaleError) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // First statement successfully targets one shard, selecing it as the coordinator.

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Start a subsequent statement that targets two new shards and encounters a stale error from at
    // least one of them.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd);

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find", kDummyStatus));
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort2, hostAndPort3}, getSessionId(), txnNum);
    future.default_timed_get();

    // Shards 2 and 3 must start a transaction, but shard 1 must not.
    ASSERT_FALSE(
        txnRouter
            .attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd)["startTransaction"]
            .trueValue());
    ASSERT_TRUE(
        txnRouter
            .attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd)["startTransaction"]
            .trueValue());
    ASSERT_TRUE(
        txnRouter
            .attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd)["startTransaction"]
            .trueValue());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RetriesCannotPickNewAtClusterTimeOnStatementAfterSelected) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    // First statement selects an atClusterTime.

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // A later statement retries on a stale version error and a view resolution error and cannot
    // change the atClusterTime.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    VectorClock::get(operationContext())->advanceClusterTime_forTest(laterTime);

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find", kDummyStatus));
    txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp());
    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard2,
                                                    BSON("find"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());

    auto future =
        launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort2}, getSessionId(), txnNum);
    future.default_timed_get();

    txnRouter.setDefaultAtClusterTime(operationContext());

    newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                               shard2,
                                               BSON("find"
                                                    << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession, WritesCanOnlyBeRetriedIfFirstOverallCommand) {
    auto writeCmds = {"insert", "update", "delete", "findAndModify", "findandmodify"};
    auto otherCmds = {"find", "distinct", "aggregate", "killCursors", "getMore"};

    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    for (auto writeCmd : writeCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(writeCmd, kDummyStatus));
    }

    for (auto cmd : otherCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(cmd, kDummyStatus));
    }

    // Advance to the next command.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    for (auto writeCmd : writeCmds) {
        ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError(writeCmd, kDummyStatus));
    }

    for (auto cmd : otherCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(cmd, kDummyStatus));
    }
}

TEST_F(TransactionRouterTest, AbortThrowsIfNoParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_THROWS_CODE(
        txnRouter.abortTransaction(opCtx), DBException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, AbortForSingleParticipant) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    auto future = launchAsync([&] { return txnRouter.abortTransaction(operationContext()); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true /* isCoordinator */);

        return kOkReadOnlyFalseResponse;
    });

    auto response = future.default_timed_get();
    ASSERT_BSONOBJ_EQ(kOkReadOnlyFalseResponse, response);
}

TEST_F(TransactionRouterTest, AbortForMultipleParticipantsAllReturnSuccess) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);

    auto future = launchAsync([&] { return txnRouter.abortTransaction(operationContext()); });

    std::map<HostAndPort, boost::optional<bool>> targets = {{hostAndPort1, true},
                                                            {hostAndPort2, {}}};

    while (!targets.empty()) {
        onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
            auto target = targets.find(request.target);
            ASSERT(target != targets.end());
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "abortTransaction");

            checkSessionDetails(request.cmdObj, lsid, txnNum, target->second);

            targets.erase(request.target);
            return kOkReadOnlyFalseResponse;
        });
    }

    auto response = future.default_timed_get();
    ASSERT_BSONOBJ_EQ(kOkReadOnlyFalseResponse, response);
}

TEST_F(TransactionRouterTest, AbortForMultipleParticipantsSomeReturnNoSuchTransaction) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard3, kOkReadOnlyFalseResponse);

    auto future = launchAsync([&] { return txnRouter.abortTransaction(operationContext()); });

    std::map<HostAndPort, boost::optional<bool>> targets = {
        {hostAndPort1, true}, {hostAndPort2, {}}, {hostAndPort3, {}}};

    int count = 0;
    while (!targets.empty()) {
        onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
            auto target = targets.find(request.target);
            ASSERT(target != targets.end());
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "abortTransaction");

            checkSessionDetails(request.cmdObj, lsid, txnNum, target->second);

            targets.erase(request.target);

            // The middle response is NoSuchTransaction, the rest are success.
            return (count == 1 ? kNoSuchTransactionResponse : kOkReadOnlyFalseResponse);
        });
        count++;
    }

    auto response = future.default_timed_get();
    ASSERT_BSONOBJ_EQ(kNoSuchTransactionResponse, response);
}

TEST_F(TransactionRouterTest, AbortForMultipleParticipantsSomeReturnNetworkError) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard3, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(operationContext(), shard3, kOkReadOnlyFalseResponse);

    auto future = launchAsync([&] { return txnRouter.abortTransaction(operationContext()); });

    std::map<HostAndPort, boost::optional<bool>> targets = {
        {hostAndPort1, true}, {hostAndPort2, {}}, {hostAndPort3, {}}};

    int count = 0;
    while (!targets.empty()) {
        onCommandForPoolExecutor([&](const RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            auto target = targets.find(request.target);
            ASSERT(target != targets.end());
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "abortTransaction");

            checkSessionDetails(request.cmdObj, lsid, txnNum, target->second);

            targets.erase(request.target);

            // The middle response is a "network error", the rest are success. Use InternalError as
            // the "network error" because the server will retry three times on actual network
            // errors; this just skips the retries.
            if (count == 1) {
                return Status{ErrorCodes::InternalError, "dummy"};
            }
            return kOkReadOnlyFalseResponse;
        });
        count++;
    }

    ASSERT_THROWS_CODE(future.default_timed_get(), AssertionException, ErrorCodes::InternalError);
}

TEST_F(TransactionRouterTestWithDefaultSession, OnViewResolutionErrorClearsAllNewParticipants) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // One shard is targeted by the first statement.
    auto firstShardCmd =
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_TRUE(firstShardCmd["startTransaction"].trueValue());

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate a view resolution error on the first client statement that leads to a retry which
    // targets the same shard.

    auto future =
        launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.default_timed_get();

    // The only participant was the coordinator, so it should have been reset.
    ASSERT_FALSE(txnRouter.getCoordinatorId());

    // The first shard is targeted by the retry and should have to start a transaction again.
    firstShardCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_TRUE(firstShardCmd["startTransaction"].trueValue());

    // Advance to a later client statement that targets a new shard.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    auto secondShardCmd =
        txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    ASSERT_TRUE(secondShardCmd["startTransaction"].trueValue());

    // Simulate a view resolution error.

    future = launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort2}, getSessionId(), txnNum);
    future.default_timed_get();

    // Only the new participant shard was reset.
    firstShardCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_FALSE(firstShardCmd["startTransaction"].trueValue());
    secondShardCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    ASSERT_TRUE(secondShardCmd["startTransaction"].trueValue());
}

TEST_F(TransactionRouterTest, ImplicitAbortIsNoopWithNoParticipants) {
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Should not throw.
    txnRouter.implicitlyAbortTransaction(opCtx, kDummyStatus);
}

TEST_F(TransactionRouterTest, ImplicitAbortForSingleParticipant) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    auto future = launchAsync(
        [&] { return txnRouter.implicitlyAbortTransaction(operationContext(), kDummyStatus); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true /* isCoordinator */);

        return kOkReadOnlyFalseResponse;
    });

    future.default_timed_get();
}

TEST_F(TransactionRouterTest, ImplicitAbortForMultipleParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    auto future = launchAsync(
        [&] { return txnRouter.implicitlyAbortTransaction(operationContext(), kDummyStatus); });

    std::map<HostAndPort, boost::optional<bool>> targets = {{hostAndPort1, true},
                                                            {hostAndPort2, {}}};

    while (!targets.empty()) {
        onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
            auto target = targets.find(request.target);
            ASSERT(target != targets.end());
            ASSERT_EQ("admin", request.dbname);

            auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
            ASSERT_EQ(cmdName, "abortTransaction");

            checkSessionDetails(request.cmdObj, lsid, txnNum, target->second);

            targets.erase(request.target);
            return kOkReadOnlyFalseResponse;
        });
    }

    future.default_timed_get();
}

TEST_F(TransactionRouterTest, ImplicitAbortIgnoresErrors) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    auto future = launchAsync(
        [&] { return txnRouter.implicitlyAbortTransaction(operationContext(), kDummyStatus); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true /* isCoordinator */);

        return BSON("ok" << 0);
    });

    // Shouldn't throw.
    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession, AbortPropagatesWriteConcern) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();
    auto txnRouter = TransactionRouter::get(opCtx);

    WriteConcernOptions writeConcern(
        10, WriteConcernOptions::SyncMode::NONE, WriteConcernOptions::kNoTimeout);
    opCtx->setWriteConcern(writeConcern);

    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.setDefaultAtClusterTime(opCtx);
    txnRouter.attachTxnFieldsIfNeeded(opCtx, shard1, kDummyFindCmd);

    auto future = launchAsync([&] { return txnRouter.abortTransaction(operationContext()); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkWriteConcern(request.cmdObj, writeConcern);

        return kOkReadOnlyFalseResponse;
    });

    auto response = future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession, ContinueOnlyOnStaleVersionOnFirstOp) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    disableRouterRetriesFailPoint();

    // Cannot retry on snapshot errors on the first statement.
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());

    // Retry only on first op on stale shard or db version errors for read or write commands.
    ASSERT_TRUE(txnRouter.canContinueOnStaleShardOrDbError("find", kStaleConfigStatus));
    ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError("insert", kDummyStatus));

    // It shouldn't hang because there won't be any abort transaction sent at this time
    txnRouter.onStaleShardOrDbError(operationContext(), "find", kStaleConfigStatus);

    // Readd the initial participant removed on onStaleShardOrDbError
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // Add another participant
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    // Check that the transaction cannot continue on stale config with more than one participant
    ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError("update", kStaleConfigStatus));

    // Can still continue on view resolution errors.
    auto future = launchAsync([&] {
        // Should not throw.
        txnRouter.onViewResolutionError(operationContext(), kViewNss);
    });

    // Expect abort on pending operation
    expectAbortTransactions({hostAndPort1, hostAndPort2}, getSessionId(), txnNum);

    future.default_timed_get();

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

    // Start a new transaction statement.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    // Cannot retry on a stale config error with one participant after the first statement.
    ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError("update", kStaleConfigStatus));
}

TEST_F(TransactionRouterTestWithDefaultSession, ContinuingTransactionPlacesItsReadConcernOnOpCtx) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    ASSERT(repl::ReadConcernArgs::get(operationContext()).getLevel() ==
           repl::ReadConcernLevel::kSnapshotReadConcern);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SubsequentStatementCanSelectAtClusterTimeIfNotSelectedYet) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    // First statement does not select an atClusterTime, but does not target any participants.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    // Subsequent statement does select an atClusterTime and does target a participant.
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime" << kInMemoryLogicalTime.asTimestamp());

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("insert"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());

    // The next statement cannot change the atClusterTime.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    LogicalTime laterTimeSameStmt(Timestamp(100, 1));
    ASSERT_GT(laterTimeSameStmt, kInMemoryLogicalTime);
    VectorClock::get(operationContext())->advanceClusterTime_forTest(laterTimeSameStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                               shard2,
                                               BSON("insert"
                                                    << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession, NonSnapshotReadConcernHasNoAtClusterTime) {
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(rcIt.second);

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);

        // No atClusterTime is placed on the router by default.
        ASSERT_FALSE(txnRouter.mustUseAtClusterTime());

        // Can't compute and set an atClusterTime.
        txnRouter.setDefaultAtClusterTime(operationContext());
        ASSERT_FALSE(txnRouter.mustUseAtClusterTime());

        // Can't continue on snapshot errors.
        ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SupportedNonSnapshotReadConcernLevelsArePassedThrough) {
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(rcIt.second);

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        const BSONObj expectedRC = BSON("level" << rcIt.first);
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedRC, newCmd["readConcern"].Obj());

        // Only attached on first command to a participant.
        newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                   shard1,
                                                   BSON("insert"
                                                        << "test"));
        ASSERT(newCmd["readConcern"].eoo());

        // Attached for new participants after the first one.
        newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                   shard2,
                                                   BSON("insert"
                                                        << "test"));
        ASSERT_BSONOBJ_EQ(expectedRC, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       NonSnapshotReadConcernLevelsPreserveAfterClusterTime) {
    const auto clusterTime = LogicalTime(Timestamp(10, 1));
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(clusterTime, rcIt.second);

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(
            BSON("level" << rcIt.first << "afterClusterTime" << clusterTime.asTimestamp()),
            newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, NonSnapshotReadConcernLevelsPreserveAfterOpTime) {
    const auto opTime = repl::OpTime(Timestamp(10, 1), 2);
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(opTime, rcIt.second);

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);

        // Call setDefaultAtClusterTime to simulate real command execution.
        txnRouter.setDefaultAtClusterTime(operationContext());

        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                        shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("level" << rcIt.first << "afterOpTime" << opTime),
                          newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesIgnoresNoSuchTransaction) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // NoSuchTransaction is ignored when it is the top-level error code.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });

    auto noSuchTransactionError = [&] {
        BSONObjBuilder bob;
        CommandHelpers::appendCommandStatusNoThrow(bob,
                                                   Status(ErrorCodes::NoSuchTransaction, "dummy"));
        return bob.obj();
    }();

    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum, noSuchTransactionError);

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesUsesIdempotentRetryPolicy) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // Retryable top-level error.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });

    auto retryableError = [&] {
        BSONObjBuilder bob;
        CommandHelpers::appendCommandStatusNoThrow(
            bob, Status(ErrorCodes::InterruptedDueToReplStateChange, "dummy"));
        return bob.obj();
    }();

    // The first abort fails with a retryable error, which should be retried.
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum, retryableError);
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);

    future.default_timed_get();
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesFailsWithNoSuchTransactionOnUnexpectedErrors) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // Non-retryable top-level error.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] {
        ASSERT_THROWS_CODE(txnRouter.onSnapshotError(operationContext(), kDummyStatus),
                           AssertionException,
                           ErrorCodes::NoSuchTransaction);
    });
    auto abortError = [&] {
        BSONObjBuilder bob;
        CommandHelpers::appendCommandStatusNoThrow(bob, Status(ErrorCodes::InternalError, "dummy"));
        return bob.obj();
    }();
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum, abortError);

    future.default_timed_get();
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             ProcessParticipantResponseInvariantsIfParticipantDoesNotExist,
             "Participant should exist if processing participant response") {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Add some participants to the list.
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    // Simulate response from some participant not in the list.
    txnRouter.processParticipantResponse(operationContext(), shard3, kOkReadOnlyTrueResponse);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseDoesNotUpdateParticipantIfResponseStatusIsNotOk) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, BSON("ok" << 0));
    ASSERT(TransactionRouter::Participant::ReadOnly::kUnset ==
           txnRouter.getParticipant(shard1)->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseMarksParticipantAsReadOnlyIfResponseSaysReadOnlyTrue) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);

    const auto participant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);

    // Further responses with readOnly: true do not change the participant's readOnly field.

    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);

    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseMarksParticipantAsNotReadOnlyIfFirstResponseSaysReadOnlyFalse) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);

    const auto participant = txnRouter.getParticipant(shard1);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    // Further responses with readOnly: false do not change the participant's readOnly field.

    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);
}

TEST_F(
    TransactionRouterTestWithDefaultSession,
    ProcessParticipantResponseUpdatesParticipantToReadOnlyFalseIfLaterResponseSaysReadOnlyFalse) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // First response says readOnly: true.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);

    const auto oldParticipant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == oldParticipant->readOnly);

    // Later response says readOnly: false.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);

    const auto updatedParticipant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == updatedParticipant->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseThrowsIfParticipantClaimsToChangeFromReadOnlyFalseToReadOnlyTrue) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // First response says readOnly: false.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);

    const auto participant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    // Later response says readOnly: true.
    ASSERT_THROWS_CODE(
        txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse),
        AssertionException,
        51113);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseThrowsIfParticipantReturnsErrorThenSuccessOnLaterStatement) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);

    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);

    // First response is an error.
    txnRouter.processParticipantResponse(operationContext(), shard1, BSON("ok" << 0));
    const auto participant = txnRouter.getParticipant(shard1);
    ASSERT(TransactionRouter::Participant::ReadOnly::kUnset == participant->readOnly);

    // The client should normally not issue another statement for the transaction, but if the client
    // does and the participant returns success for some reason, the router should throw.

    // Reset the readConcern on the OperationContext to simulate a new request.
    repl::ReadConcernArgs secondRequestEmptyReadConcern;
    repl::ReadConcernArgs::get(operationContext()) = secondRequestEmptyReadConcern;

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    // The router should throw regardless of whether the response says readOnly true or false.
    ASSERT_THROWS_CODE(
        txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse),
        AssertionException,
        51112);
    ASSERT_THROWS_CODE(
        txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse),
        AssertionException,
        51112);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantSkipsValidationIfAbortAlreadyInitiated) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.setDefaultAtClusterTime(opCtx);
    txnRouter.attachTxnFieldsIfNeeded(opCtx, shard1, kDummyFindCmd);

    // Continue causes the _latestStmtId to be bumped.
    repl::ReadConcernArgs::get(opCtx) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kContinue);

    // Aborting will set the termination initiation state.
    auto future = launchAsync([&] { txnRouter.abortTransaction(opCtx); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.default_timed_get();

    // The participant's response metadata should not be processed since abort has been initiated.
    txnRouter.processParticipantResponse(operationContext(), shard1, BSON("ok" << 0));
    ASSERT(TransactionRouter::Participant::ReadOnly::kUnset ==
           txnRouter.getParticipant(shard1)->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantSkipsValidationIfImplicitAbortAlreadyInitiated) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.setDefaultAtClusterTime(opCtx);
    txnRouter.attachTxnFieldsIfNeeded(opCtx, shard1, kDummyFindCmd);

    // Aborting will set the termination initiation state.
    auto future = launchAsync([&] { txnRouter.implicitlyAbortTransaction(opCtx, kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.default_timed_get();

    // The participant's response metadata should not be processed since abort has been initiated.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kUnset ==
           txnRouter.getParticipant(shard1)->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantSkipsValidationIfCommitAlreadyInitiated) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.setDefaultAtClusterTime(opCtx);
    txnRouter.attachTxnFieldsIfNeeded(opCtx, shard1, kDummyFindCmd);

    // Process !readonly response to set participant state.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly ==
           txnRouter.getParticipant(shard1)->readOnly);

    // Commit causes the _latestStmtId to be bumped.
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    // Committing will set the termination initiation state.
    auto future = launchAsync([&] { txnRouter.commitTransaction(opCtx, boost::none); });
    expectCommitTransaction();
    future.default_timed_get();

    // Processing readonly response should not throw since commit has been initiated.
    txnRouter.processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotAdvanceTxnNumberWithActiveYielder) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.stash(opCtx, TransactionRouter::StashReason::kYield);

    // We can beginOrContinueTxn at the active txnNumber. This simulates a yielded session being
    // checked out by a different operation for that same transaction.
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kContinue);

    txnRouter.stash(opCtx, TransactionRouter::StashReason::kYield);

    // A higher txnNumber cannot be used.
    ASSERT_THROWS_CODE(txnRouter.beginOrContinueTxn(
                           opCtx, txnNum + 1, TransactionRouter::TransactionActions::kStart),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    ASSERT_THROWS_CODE(txnRouter.beginOrContinueTxn(
                           opCtx, txnNum + 1, TransactionRouter::TransactionActions::kCommit),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    TransactionRouter::get(opCtx).unstash(opCtx);

    // A higher txnNumber still cannot be used because there is an outstanding yielder.
    ASSERT_THROWS_CODE(txnRouter.beginOrContinueTxn(
                           opCtx, txnNum + 1, TransactionRouter::TransactionActions::kStart),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    ASSERT_THROWS_CODE(txnRouter.beginOrContinueTxn(
                           opCtx, txnNum + 1, TransactionRouter::TransactionActions::kCommit),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);

    TransactionRouter::get(opCtx).unstash(opCtx);

    // A non-yielding stash does not affect whether a new transaction can begin. This simulates an
    // operation completing. Stash multiple times to verify it does not bring the activeYields
    // counter below 0 and trigger a crash.
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);

    // Now the router can advance.
    txnRouter.beginOrContinueTxn(opCtx, txnNum + 1, TransactionRouter::TransactionActions::kStart);
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             ActiveYieldersCannotGoBelowZero,
             "Invalid activeYields") {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);

    txnRouter.stash(opCtx, TransactionRouter::StashReason::kYield);
    txnRouter.unstash(opCtx);
    txnRouter.unstash(opCtx);
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotBeReapedWithActiveYielders) {
    TxnNumber txnNum{3};
    operationContext()->setTxnNumber(txnNum);
    auto opCtx = operationContext();

    auto txnRouter = TransactionRouter::get(opCtx);

    // Can be reaped initially.
    ASSERT(txnRouter.canBeReaped());

    // Can be reaped after a non-yield stash.
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);
    ASSERT(txnRouter.canBeReaped());

    // Cannot be reaped after a yield stash.
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kContinue);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kYield);
    ASSERT_FALSE(txnRouter.canBeReaped());

    // Cannot be reaped after multiple yield stashes until all corresponding unstashes.
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kContinue);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kYield);
    ASSERT_FALSE(txnRouter.canBeReaped());

    txnRouter.unstash(opCtx);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);
    ASSERT_FALSE(txnRouter.canBeReaped());

    txnRouter.unstash(opCtx);
    txnRouter.stash(opCtx, TransactionRouter::StashReason::kDone);
    ASSERT(txnRouter.canBeReaped());
}

// Begins a transaction with snapshot level read concern and sets a default cluster time.
class TransactionRouterTestWithDefaultSessionAndStartedSnapshot
    : public TransactionRouterTestWithDefaultSession {
protected:
    const TxnNumber kTxnNumber = 10;
    const BSONObj rcLatestInMemoryAtClusterTime = BSON("level"
                                                       << "snapshot"
                                                       << "atClusterTime"
                                                       << kInMemoryLogicalTime.asTimestamp());

    void setUp() override {
        TransactionRouterTestWithDefaultSession::setUp();

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }
};

TEST_F(TransactionRouterTestWithDefaultSessionAndStartedSnapshot, AddAtClusterTimeNormal) {
    auto txnRouter = TransactionRouter::get(operationContext());
    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("aggregate"
                                                         << "testColl"
                                                         << "readConcern"
                                                         << BSON("level"
                                                                 << "snapshot")));

    ASSERT_BSONOBJ_EQ(rcLatestInMemoryAtClusterTime, newCmd["readConcern"].Obj());
}

TEST_F(TransactionRouterTestWithDefaultSessionAndStartedSnapshot,
       AddingAtClusterTimeOverwritesExistingAfterClusterTime) {
    const Timestamp existingAfterClusterTime(1, 1);

    auto txnRouter = TransactionRouter::get(operationContext());
    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(operationContext(),
                                                    shard1,
                                                    BSON("aggregate"
                                                         << "testColl"
                                                         << "readConcern"
                                                         << BSON("level"
                                                                 << "snapshot"
                                                                 << "afterClusterTime"
                                                                 << existingAfterClusterTime)));

    ASSERT_BSONOBJ_EQ(rcLatestInMemoryAtClusterTime, newCmd["readConcern"].Obj());
}

/**
 * Test fixture for router transactions metrics.
 */
class TransactionRouterMetricsTest : public TransactionRouterTestWithDefaultSession {
protected:
    using CommitType = TransactionRouter::CommitType;

    const TxnNumber kTxnNumber = 10;
    const TxnRecoveryToken kDummyRecoveryToken;

    static constexpr auto kDefaultTimeActive = Microseconds(50);
    static constexpr auto kDefaultTimeInactive = Microseconds(100);

    void setUp() override {
        TransactionRouterTestWithDefaultSession::setUp();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
        _txnRouter = TransactionRouter::get(operationContext());
        operationContext()->setTxnNumber(kTxnNumber);
    }

    TickSourceMock<Microseconds>* tickSource() {
        return dynamic_cast<TickSourceMock<Microseconds>*>(getServiceContext()->getTickSource());
    }

    /**
     * Set up and return a mock clock source.
     */
    ClockSourceMock* preciseClockSource() {
        getServiceContext()->setPreciseClockSource(std::make_unique<ClockSourceMock>());
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    TransactionRouter::Router& txnRouter() {
        invariant(_txnRouter);
        return *_txnRouter;
    }

    void beginTxnWithDefaultTxnNumber() {
        txnRouter().beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter().setDefaultAtClusterTime(operationContext());
    }

    void beginSlowTxnWithDefaultTxnNumber() {
        txnRouter().beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter().setDefaultAtClusterTime(operationContext());
        tickSource()->advance(Milliseconds(serverGlobalParams.slowMS + 1));
    }

    void beginRecoverCommitWithDefaultTxnNumber() {
        txnRouter().beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
        txnRouter().setDefaultAtClusterTime(operationContext());
    }

    void beginSlowRecoverCommitWithDefaultTxnNumber() {
        txnRouter().beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
        txnRouter().setDefaultAtClusterTime(operationContext());
        tickSource()->advance(Milliseconds(serverGlobalParams.slowMS + 1));
    }

    void assertDurationIs(Microseconds micros) {
        auto stats = txnRouter().getTimingStats_forTest();
        ASSERT_EQ(stats.getDuration(tickSource(), tickSource()->getTicks()), micros);
    }

    void assertCommitDurationIs(Microseconds micros) {
        auto stats = txnRouter().getTimingStats_forTest();
        ASSERT_EQ(stats.getCommitDuration(tickSource(), tickSource()->getTicks()), micros);
    }

    bool networkHasReadyRequests() {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        return guard->hasReadyRequests();
    }

    void assertTimeActiveIs(Microseconds micros) {
        auto stats = txnRouter().getTimingStats_forTest();
        ASSERT_EQ(stats.getTimeActiveMicros(tickSource(), tickSource()->getTicks()), micros);
    }

    void assertTimeInactiveIs(Microseconds micros) {
        auto stats = txnRouter().getTimingStats_forTest();
        ASSERT_EQ(stats.getTimeInactiveMicros(tickSource(), tickSource()->getTicks()), micros);
    }

    //
    // Helpers for each way a router's transaction may terminate. Meant to be used where the
    // particular commit type is not being tested.
    //

    void explicitAbortInProgress() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard1, kOkReadOnlyFalseResponse);

        startCapturingLogMessages();
        auto future = launchAsync([&] { txnRouter().abortTransaction(operationContext()); });
        expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void implicitAbortInProgress() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard1, kOkReadOnlyFalseResponse);

        startCapturingLogMessages();
        auto future = launchAsync(
            [&] { txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus); });
        expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void runCommit(StatusWith<BSONObj> swRes, bool expectRetries = false) {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard1, kOkReadOnlyFalseResponse);

        startCapturingLogMessages();
        auto future = launchAsync([&] {
            if (swRes.isOK()) {
                txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);
            } else {
                ASSERT_THROWS_CODE(
                    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken),
                    AssertionException,
                    swRes.getStatus().code());
            }
        });
        // commitTransaction() uses the ARS, which retries on retryable errors up to 3 times.
        int expectedAttempts = expectRetries ? 4 : 1;
        for (int i = 0; i < expectedAttempts; i++) {
            expectCommitTransaction(swRes);
        }
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void retryCommit(StatusWith<BSONObj> swRes, bool expectRetries = false) {
        startCapturingLogMessages();
        auto future = launchAsync([&] {
            if (swRes.isOK()) {
                txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);
            } else {
                ASSERT_THROWS_CODE(
                    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken),
                    AssertionException,
                    swRes.getStatus().code());
            }
        });
        // commitTransaction() uses the ARS, which retries on retryable errors up to 3 times.
        int expectedAttempts = expectRetries ? 4 : 1;
        for (int i = 0; i < expectedAttempts; i++) {
            expectCommitTransaction(swRes);
        }
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    //
    // Helpers for running each kind of commit.
    //

    void runNoShardCommit() {
        startCapturingLogMessages();
        txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);
        stopCapturingLogMessages();
    }

    void runSingleShardCommit() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);

        startCapturingLogMessages();
        auto future = launchAsync(
            [&] { txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken); });
        expectCommitTransaction();
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void runReadOnlyCommit() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        txnRouter().processParticipantResponse(operationContext(), shard2, kOkReadOnlyTrueResponse);

        startCapturingLogMessages();
        auto future = launchAsync(
            [&] { txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken); });
        expectCommitTransaction();
        expectCommitTransaction();
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void runSingleWriteShardCommit() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyTrueResponse);
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard2, kOkReadOnlyFalseResponse);

        startCapturingLogMessages();
        auto future = launchAsync(
            [&] { txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken); });
        expectCoordinateCommitTransaction();
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void runTwoPhaseCommit() {
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard1, kOkReadOnlyFalseResponse);
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard2, kOkReadOnlyFalseResponse);

        startCapturingLogMessages();
        auto future = launchAsync(
            [&] { txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken); });
        expectCoordinateCommitTransaction();
        future.default_timed_get();
        stopCapturingLogMessages();
    }

    void runRecoverWithTokenCommit(boost::optional<ShardId> recoveryShard) {
        txnRouter().beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);

        TxnRecoveryToken recoveryToken;
        recoveryToken.setRecoveryShardId(recoveryShard);

        startCapturingLogMessages();
        if (recoveryShard) {
            auto future = launchAsync(
                [&] { txnRouter().commitTransaction(operationContext(), recoveryToken); });
            expectCoordinateCommitTransaction();
            future.default_timed_get();
        } else {
            ASSERT_THROWS_CODE(txnRouter().commitTransaction(operationContext(), recoveryToken),
                               AssertionException,
                               ErrorCodes::NoSuchTransaction);
        }
        stopCapturingLogMessages();
    }

    //
    // Miscellaneous methods.
    //

    auto beginAndPauseCommit() {
        // Commit after targeting one shard so the commit has to do work and can be paused.
        txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
        txnRouter().processParticipantResponse(
            operationContext(), shard1, kOkReadOnlyFalseResponse);
        auto future = launchAsync(
            [&] { txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken); });

        while (!networkHasReadyRequests()) {
            // Wait for commit to start.
        }
        return future;
    }

    void assertPrintedExactlyOneSlowLogLine() {
        ASSERT_EQUALS(1, countTextFormatLogLinesContaining("transaction"));
    }

    void assertDidNotPrintSlowLogLine() {
        ASSERT_EQUALS(0, countTextFormatLogLinesContaining("transaction"));
    }

    auto routerTxnMetrics() {
        return RouterTransactionsMetrics::get(operationContext());
    }

    void assertTimeActiveAndInactiveCannotAdvance(Microseconds timeActive,
                                                  Microseconds timeInactive) {
        tickSource()->advance(Microseconds(150));
        assertTimeActiveIs(Microseconds(timeActive));
        assertTimeInactiveIs(Microseconds(timeInactive));

        txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);

        tickSource()->advance(Microseconds(150));
        assertTimeActiveIs(Microseconds(timeActive));
        assertTimeInactiveIs(Microseconds(timeInactive));
    }

    void setUpDefaultTimeActiveAndInactive() {
        tickSource()->advance(kDefaultTimeActive);
        assertTimeActiveIs(kDefaultTimeActive);
        assertTimeInactiveIs(Microseconds(0));

        txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);

        tickSource()->advance(kDefaultTimeInactive);
        assertTimeActiveIs(kDefaultTimeActive);
        assertTimeInactiveIs(kDefaultTimeInactive);
    }

private:
    boost::optional<TransactionRouter::Router> _txnRouter;
};

//
// Slow transaction logging tests that logging obeys configuration options and only logs once per
// transaction.
//

TEST_F(TransactionRouterMetricsTest, DoesNotLogTransactionsUnderSlowMSThreshold) {
    const auto originalSlowMS = serverGlobalParams.slowMS;
    const auto originalSampleRate = serverGlobalParams.sampleRate;

    serverGlobalParams.slowMS = 100;
    serverGlobalParams.sampleRate = 1;

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS = originalSlowMS;
        serverGlobalParams.sampleRate = originalSampleRate;
    });

    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Milliseconds(99));
    runCommit(kDummyOkRes);
    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, LogsTransactionsOverSlowMSThreshold) {
    const auto originalSlowMS = serverGlobalParams.slowMS;
    const auto originalSampleRate = serverGlobalParams.sampleRate;

    serverGlobalParams.slowMS = 100;
    serverGlobalParams.sampleRate = 1;

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS = originalSlowMS;
        serverGlobalParams.sampleRate = originalSampleRate;
    });

    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Milliseconds(101));
    runCommit(kDummyOkRes);
    assertPrintedExactlyOneSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, LogsTransactionsWithAPIParameters) {
    const auto originalSlowMS = serverGlobalParams.slowMS;
    const auto originalSampleRate = serverGlobalParams.sampleRate;

    serverGlobalParams.slowMS = 100;
    serverGlobalParams.sampleRate = 1;

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS = originalSlowMS;
        serverGlobalParams.sampleRate = originalSampleRate;
    });

    APIParameters::get(operationContext()).setAPIVersion("1");
    APIParameters::get(operationContext()).setAPIStrict(true);
    APIParameters::get(operationContext()).setAPIDeprecationErrors(false);
    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Milliseconds(101));
    runCommit(kDummyOkRes);
    assertPrintedExactlyOneSlowLogLine();

    int nFound = 0;
    for (auto&& bson : getCapturedBSONFormatLogMessages()) {
        if (bson["id"].Int() != 51805) {
            continue;
        }

        auto parameters = bson["attr"]["parameters"];
        ASSERT_EQUALS(parameters["apiVersion"].String(), "1");
        ASSERT_EQUALS(parameters["apiStrict"].Bool(), true);
        ASSERT_EQUALS(parameters["apiDeprecationErrors"].Bool(), false);
        ++nFound;
    }

    ASSERT_EQUALS(nFound, 1);
}

TEST_F(TransactionRouterMetricsTest, DoesNotLogTransactionsWithSampleRateZero) {
    const auto originalSlowMS = serverGlobalParams.slowMS;
    const auto originalSampleRate = serverGlobalParams.sampleRate;

    serverGlobalParams.slowMS = 100;
    serverGlobalParams.sampleRate = 0;

    // Reset the global parameters to their original values after this test exits.
    ON_BLOCK_EXIT([originalSlowMS, originalSampleRate] {
        serverGlobalParams.slowMS = originalSlowMS;
        serverGlobalParams.sampleRate = originalSampleRate;
    });

    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Milliseconds(101));
    runCommit(kDummyOkRes);
    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, OnlyLogSlowTransactionsOnce) {
    beginSlowTxnWithDefaultTxnNumber();

    startCapturingLogMessages();

    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);
    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);
    ASSERT_THROWS(txnRouter().abortTransaction(operationContext()), AssertionException);

    stopCapturingLogMessages();

    assertPrintedExactlyOneSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoTransactionsLoggedAtDefaultTransactionLogLevel) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    beginTxnWithDefaultTxnNumber();
    runSingleShardCommit();
    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, AllTransactionsLoggedAtTransactionLogLevelOne) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Debug(1)};
    beginTxnWithDefaultTxnNumber();
    runSingleShardCommit();
    assertPrintedExactlyOneSlowLogLine();
}

//
// Slow transaction logging tests for the logging of basic transaction parameters.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingPrintsTransactionParameters) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    BSONObjBuilder lsidBob;
    getSessionId().serialize(&lsidBob);

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("parameters" << BSON("lsid" << lsidBob.obj() << "txnNumber" << kTxnNumber
                                                       << "autocommit" << false)))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingPrintsDurationAtEnd) {
    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Milliseconds(111));
    assertDurationIs(Milliseconds(111));
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("durationMillis" << 111))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingPrintsTimeActiveAndInactive) {
    beginTxnWithDefaultTxnNumber();
    tickSource()->advance(Microseconds(111111));
    assertTimeActiveIs(Microseconds(111111));

    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    tickSource()->advance(Microseconds(222222));
    assertTimeInactiveIs(Microseconds(222222));

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(
        1, countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("timeActiveMicros" << 111111))));
    ASSERT_EQUALS(
        1, countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("timeInactiveMicros" << 222222))));
}

//
// Slow transaction logging tests for API parameters.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingAPIParameters) {
    APIParameters apiParams = APIParameters();
    apiParams.setAPIVersion("2");
    apiParams.setAPIStrict(true);
    apiParams.setAPIDeprecationErrors(true);

    APIParameters::get(operationContext()) = apiParams;

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON(
                      "attr" << BSON("parameters" << BSON("apiVersion"
                                                          << "2"
                                                          << "apiStrict" << true
                                                          << "apiDeprecationErrors" << true)))));
}

//
// Slow transaction logging tests for the parameters that depend on the read concern level.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingReadConcern_None) {
    auto readConcern = repl::ReadConcernArgs();
    repl::ReadConcernArgs::get(operationContext()) = readConcern;

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining(readConcern.toBSON()["readConcern"]));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("globalReadTimestamp:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingReadConcern_Local) {
    auto readConcern = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    repl::ReadConcernArgs::get(operationContext()) = readConcern;

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("parameters"
                           << BSON("readConcern" << readConcern.toBSON()["readConcern"].Obj())))));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("globalReadTimestamp:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingReadConcern_Majority) {
    auto readConcern = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    repl::ReadConcernArgs::get(operationContext()) = readConcern;

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("parameters"
                           << BSON("readConcern" << readConcern.toBSON()["readConcern"].Obj())))));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("globalReadTimestamp:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingReadConcern_Snapshot) {
    auto readConcern = repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    repl::ReadConcernArgs::get(operationContext()) = readConcern;

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(
            BSON("attr" << BSON("parameters"
                                << BSON("readConcern" << readConcern.toBSON()["readConcern"].Obj()))
                        << "globalReadTimestamp" << BSONUndefined)));
}

//
// Slow transaction logging tests for the fields that correspond to commit type.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_NoShards) {
    beginSlowTxnWithDefaultTxnNumber();
    runNoShardCommit();

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("commitType"
                           << "noShards"
                           << "numParticipants" << 0 << "commitDurationMicros" << BSONUndefined))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("coordinator:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_SingleShard) {
    beginSlowTxnWithDefaultTxnNumber();
    runSingleShardCommit();

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("commitType"
                           << "singleShard"
                           << "numParticipants" << 1 << "commitDurationMicros" << BSONUndefined))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("coordinator:"));
}

// TODO (SERVER-48340): Re-enable the single-write-shard transaction commit optimization.
TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_SingleWriteShard) {
    beginSlowTxnWithDefaultTxnNumber();
    runSingleWriteShardCommit();

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("commitType"
                                          << "twoPhaseCommit"
                                          << "numParticipants" << 2 << "commitDurationMicros"
                                          << BSONUndefined << "coordinator" << BSONUndefined))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_ReadOnly) {
    beginSlowTxnWithDefaultTxnNumber();
    runReadOnlyCommit();

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("commitType"
                           << "readOnly"
                           << "numParticipants" << 2 << "commitDurationMicros" << BSONUndefined))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("coordinator:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_TwoPhase) {
    beginSlowTxnWithDefaultTxnNumber();
    runTwoPhaseCommit();

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("commitType"
                                          << "twoPhaseCommit"
                                          << "numParticipants" << 2 << "commitDurationMicros"
                                          << BSONUndefined << "coordinator" << BSONUndefined))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_Recovery) {
    beginSlowRecoverCommitWithDefaultTxnNumber();
    runRecoverWithTokenCommit(shard1);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("commitType"
                                          << "recoverWithToken"
                                          << "commitDurationMicros" << BSONUndefined))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("numParticipants:"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("coordinator:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingCommitType_EmptyRecovery) {
    beginSlowRecoverCommitWithDefaultTxnNumber();
    runRecoverWithTokenCommit(boost::none);

    // Nothing is logged when recovering with an empty recovery token because we don't learn  the
    // final result of the commit.
    assertDidNotPrintSlowLogLine();
}

//
// Slow transaction logging tests for the fields that are set when a transaction terminates.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingOnTerminate_ImplicitAbort) {
    beginSlowTxnWithDefaultTxnNumber();
    implicitAbortInProgress();

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("terminationCause"
                                          << "aborted"
                                          << "abortCause" << kDummyStatus.codeString()
                                          << "numParticipants" << 1))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("commitType:"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("commitDurationMicros:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingOnTerminate_ExplicitAbort) {
    beginSlowTxnWithDefaultTxnNumber();
    explicitAbortInProgress();

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("terminationCause"
                                                                      << "aborted"
                                                                      << "abortCause"
                                                                      << "abort"
                                                                      << "numParticipants" << 1))));

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("commitType:"));
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("commitDurationMicros:"));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingOnTerminate_SuccessfulCommit) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("terminationCause"
                           << "committed"
                           << "commitType"
                           << "singleShard"
                           << "numParticipants" << 1 << "commitDurationMicros" << BSONUndefined))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingOnTerminate_FailedCommit) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyErrorRes);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("terminationCause"
                                          << "aborted"
                                          << "abortCause" << kDummyStatus.codeString()
                                          << "numParticipants" << 1 << "commitDurationMicros"
                                          << BSONUndefined << "commitType" << BSONUndefined))));

    // ASSERT_EQUALS(1, countTextFormatLogLinesContaining("commitType:"));
    // ASSERT_EQUALS(1, countTextFormatLogLinesContaining("commitDurationMicros:"));
}

//
// Slow transaction logging tests for the cases after commit where the result is unknown.
//

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_WriteConcernError) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyResWithWriteConcernError, false /* expectRetries */);

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_RetryableError) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_FailureToSend) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::CallbackCanceled, "dummy"));

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_RetryableFailureToSend) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_ExceededTimeLimit) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(BSON("ok" << 0 << "code" << ErrorCodes::MaxTimeMSExpired));

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_UnsatisfiableWriteConcern) {
    const auto resWithUnSatisfiableWriteConcernWCError = BSON(
        "ok" << 1 << "writeConcernError" << BSON("code" << ErrorCodes::UnsatisfiableWriteConcern));

    beginSlowTxnWithDefaultTxnNumber();
    runCommit(resWithUnSatisfiableWriteConcernWCError);

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnUnknownCommitResult_TransactionTooOld) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(BSON("ok" << 0 << "code" << ErrorCodes::TransactionTooOld));

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingOnImplicitAbortAfterUnknownCommitResult) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();

    // The transaction router may implicitly abort after receiving an unknown commit result error.
    // Since the transaction may have committed, it's not safe to assume the transaction will abort,
    // so nothing should be logged.
    startCapturingLogMessages();
    auto future = launchAsync(
        [&] { return txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
    future.default_timed_get();
    stopCapturingLogMessages();

    assertDidNotPrintSlowLogLine();

    retryCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingCommitAfterAbort_Failed) {
    beginSlowTxnWithDefaultTxnNumber();
    implicitAbortInProgress();

    runCommit(kDummyErrorRes);
    assertDidNotPrintSlowLogLine();
}

TEST_F(TransactionRouterMetricsTest, NoSlowLoggingCommitAfterAbort_Successful) {
    beginSlowTxnWithDefaultTxnNumber();
    implicitAbortInProgress();

    // Note that this shouldn't be possible, but is included as a test case for completeness.
    runCommit(kDummyOkRes);
    assertDidNotPrintSlowLogLine();
}

//
// Slow transaction logging tests that retrying after an unknown commit result logs if the result is
// discovered.
//

TEST_F(TransactionRouterMetricsTest, SlowLoggingAfterUnknownCommitResult_Success) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();

    retryCommit(kDummyOkRes);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("terminationCause"
                                          << "committed"
                                          << "commitDurationMicros" << BSONUndefined << "commitType"
                                          << BSONUndefined))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingAfterUnknownCommitResult_Abort) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();

    retryCommit(kDummyErrorRes);

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("terminationCause"
                                          << "aborted"
                                          << "commitDurationMicros" << BSONUndefined << "commitType"
                                          << BSONUndefined))));
}

TEST_F(TransactionRouterMetricsTest, SlowLoggingAfterUnknownCommitResult_Unknown) {
    beginSlowTxnWithDefaultTxnNumber();
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();

    retryCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    assertDidNotPrintSlowLogLine();
}

//
// Tests for the tracking of transaction timing stats.
//

TEST_F(TransactionRouterMetricsTest, DurationAdvancesAfterTransactionBegins) {
    // Advancing the clock before beginning a transaction won't affect its duration. Note that it's
    // invalid to get a transaction's duration before beginning it, so the check comes after begin.
    tickSource()->advance(Microseconds(100));

    beginTxnWithDefaultTxnNumber();

    assertDurationIs(Microseconds(0));

    // Advancing after beginning a txn will advance the duration.
    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, DurationDoesNotAdvanceAfterCommit) {
    beginTxnWithDefaultTxnNumber();

    assertDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));

    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);

    // Advancing the clock shouldn't change the duration now.
    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, DurationResetByNewTransaction) {
    beginTxnWithDefaultTxnNumber();

    assertDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));

    txnRouter().commitTransaction(operationContext(), kDummyRecoveryToken);

    // Start a new transaction and verify the duration was reset.
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);

    assertDurationIs(Microseconds(0));
    tickSource()->advance(Microseconds(50));
    assertDurationIs(Microseconds(50));
}

TEST_F(TransactionRouterMetricsTest, DurationDoesNotAdvanceAfterAbort) {
    beginTxnWithDefaultTxnNumber();

    assertDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));

    // Note this throws because there are no participants, but the transaction is still aborted.
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);

    tickSource()->advance(Microseconds(200));
    assertDurationIs(Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, DurationDoesNotAdvanceAfterImplicitAbort) {
    beginTxnWithDefaultTxnNumber();

    assertDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(100));

    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    tickSource()->advance(Microseconds(200));
    assertDurationIs(Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, CommitDurationAdvancesDuringCommit) {
    beginTxnWithDefaultTxnNumber();

    // Advancing the clock before beginning commit shouldn't affect the commit duration. Note that
    // it is invalid to get the commit duration for a transaction that hasn't tried to commit.
    tickSource()->advance(Microseconds(100));

    auto future = beginAndPauseCommit();

    // The clock hasn't advanced since commit started, so the duration should be 0.
    assertCommitDurationIs(Microseconds(0));

    // Advancing the clock during commit should increase commit duration.
    tickSource()->advance(Microseconds(100));
    assertCommitDurationIs(Microseconds(100));

    expectCommitTransaction();
    future.default_timed_get();

    // The duration shouldn't change now that commit has finished.
    tickSource()->advance(Microseconds(200));
    assertCommitDurationIs(Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, CommitDurationResetByNewTransaction) {
    beginTxnWithDefaultTxnNumber();

    tickSource()->advance(Microseconds(100));

    auto future = beginAndPauseCommit();

    assertCommitDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(100));
    assertCommitDurationIs(Microseconds(100));

    expectCommitTransaction();
    future.default_timed_get();

    // Start a new transaction and verify the commit duration was reset.
    operationContext()->setTxnNumber(kTxnNumber + 1);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);

    future = beginAndPauseCommit();

    assertCommitDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(50));
    assertCommitDurationIs(Microseconds(50));

    expectCommitTransaction();
    future.default_timed_get();

    tickSource()->advance(Microseconds(100));
    assertCommitDurationIs(Microseconds(50));
}

TEST_F(TransactionRouterMetricsTest, CommitDurationDoesNotAdvanceAfterFailedCommit) {
    beginTxnWithDefaultTxnNumber();

    auto future = beginAndPauseCommit();

    assertCommitDurationIs(Microseconds(0));

    tickSource()->advance(Microseconds(50));
    assertCommitDurationIs(Microseconds(50));

    // Commit fails with a non-retryable error.
    expectCommitTransaction(kDummyErrorRes);
    future.default_timed_get();

    // Commit duration won't advance.
    tickSource()->advance(Microseconds(100));
    assertCommitDurationIs(Microseconds(50));
}

TEST_F(TransactionRouterMetricsTest, CommitStatsNotInReportStatsForFailedCommitAfterAbort) {
    // It's a user error to commit a transaction after a failed command or explicit abort, but if it
    // happens, stats for the commit should not be tracked or included in reporting.
    beginTxnWithDefaultTxnNumber();
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    // A commit in this case should always fail.
    auto future = beginAndPauseCommit();
    tickSource()->advance(Microseconds(50));
    expectCommitTransaction(kDummyErrorRes);
    future.default_timed_get();

    auto activeState = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    ASSERT(activeState.hasField("transaction"));
    ASSERT(!activeState["transaction"].Obj().hasField("commitStartWallClockTime"));
    ASSERT(!activeState["transaction"].Obj().hasField("commitType"));

    auto inactiveState = txnRouter().reportState(operationContext(), false /* sessionIsActive */);
    ASSERT(inactiveState.hasField("transaction"));
    ASSERT(!inactiveState["transaction"].Obj().hasField("commitStartWallClockTime"));
    ASSERT(!inactiveState["transaction"].Obj().hasField("commitType"));
}

TEST_F(TransactionRouterMetricsTest, CommitStatsNotInReportStatsForSuccessfulCommitAfterAbort) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    // Note that it shouldn't be possible for this commit to succeed, but it is included as a test
    // case for completeness.
    auto future = beginAndPauseCommit();
    tickSource()->advance(Microseconds(50));
    expectCommitTransaction(kDummyOkRes);
    future.default_timed_get();

    auto activeState = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    ASSERT(activeState.hasField("transaction"));
    ASSERT(!activeState["transaction"].Obj().hasField("commitStartWallClockTime"));
    ASSERT(!activeState["transaction"].Obj().hasField("commitType"));

    auto inactiveState = txnRouter().reportState(operationContext(), false /* sessionIsActive */);
    ASSERT(inactiveState.hasField("transaction"));
    ASSERT(!inactiveState["transaction"].Obj().hasField("commitStartWallClockTime"));
    ASSERT(!inactiveState["transaction"].Obj().hasField("commitType"));
}

TEST_F(TransactionRouterMetricsTest, DurationsAdvanceAfterUnknownCommitResult) {
    beginTxnWithDefaultTxnNumber();

    tickSource()->advance(Microseconds(50));
    assertDurationIs(Microseconds(50));

    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    // Both duration and commit can still advance.
    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(150));
    assertCommitDurationIs(Microseconds(100));

    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);

    // The result is still unknown so both can advance.
    tickSource()->advance(Microseconds(100));
    assertDurationIs(Microseconds(250));
    assertCommitDurationIs(Microseconds(200));

    runCommit(kDummyOkRes);

    // The result is known, so neither can advance.
    tickSource()->advance(Microseconds(500));
    assertDurationIs(Microseconds(250));
    assertCommitDurationIs(Microseconds(200));
}

TEST_F(TransactionRouterMetricsTest, TimeActiveAndInactiveAdvanceSeparatelyAndSumToDuration) {
    beginTxnWithDefaultTxnNumber();

    // Both timeActive and timeInactive start at 0.
    assertTimeActiveIs(Microseconds(0));
    assertTimeInactiveIs(Microseconds(0));
    assertDurationIs(Microseconds(0));

    // Only timeActive will advance while a txn is active.
    tickSource()->advance(Microseconds(50));
    assertTimeActiveIs(Microseconds(50));
    assertTimeInactiveIs(Microseconds(0));
    assertDurationIs(Microseconds(50));

    // Only timeInactive will advance while a txn is stashed.
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(Microseconds(50));
    assertTimeInactiveIs(Microseconds(100));
    assertDurationIs(Microseconds(150));

    // Will not advance after commit.
    // Neither can advance after a successful commit.
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyOkRes);

    tickSource()->advance(Microseconds(150));
    assertTimeActiveIs(Microseconds(50));
    assertTimeInactiveIs(Microseconds(100));
    assertDurationIs(Microseconds(150));
}

TEST_F(TransactionRouterMetricsTest, StashIsIdempotent) {
    // An error after checking out a session and before continuing a transaction can lead to
    // back-to-back calls to TransactionRouter::stash(), so a repeated call to stash() shouldn't
    // toggle the transaction back to the active state.

    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);

    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());

    // Only timeInactive can advance.
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive + Microseconds(100));

    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());

    // Still only timeInactive can advance.
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive + Microseconds(200));
}

TEST_F(TransactionRouterMetricsTest, DurationsForImplicitlyAbortedStashedTxn) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive);

    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);

    // At shutdown transactions are implicitly aborted without being continued so a transaction may
    // be stashed when aborting, which should still lead to durations in a consistent state.
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive);
}

TEST_F(TransactionRouterMetricsTest, DurationsForImplicitlyAbortedActiveTxn) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive);

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);
    tickSource()->advance(Microseconds(100));

    assertTimeActiveIs(kDefaultTimeActive + Microseconds(100));
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive + Microseconds(100));

    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    assertTimeActiveIs(kDefaultTimeActive + Microseconds(100));
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive + Microseconds(100));
}

TEST_F(TransactionRouterMetricsTest, DurationsForImplicitlyAbortedEndedTxn) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive);

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyOkRes);
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);

    // At shutdown transactions are implicitly aborted without being continued, so an "ended"
    // transaction (i.e. committed or aborted) may be implicitly aborted again. This shouldn't
    // affect any transaction durations.
    auto future = launchAsync(
        [&] { return txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
    future.default_timed_get();

    assertTimeActiveIs(kDefaultTimeActive);
    assertTimeInactiveIs(kDefaultTimeInactive);
    assertDurationIs(kDefaultTimeActive + kDefaultTimeInactive);
}

TEST_F(TransactionRouterMetricsTest, NeitherTimeActiveNorInactiveAdvanceAfterSuccessfulCommit) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyOkRes);

    // Neither can advance.
    assertTimeActiveAndInactiveCannotAdvance(kDefaultTimeActive /*timeActive*/,
                                             kDefaultTimeInactive /*timeInactive*/);
}

TEST_F(TransactionRouterMetricsTest, NeitherTimeActiveNorInactiveAdvanceAfterFailedCommit) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);
    runCommit(kDummyErrorRes);

    // Neither can advance.
    assertTimeActiveAndInactiveCannotAdvance(kDefaultTimeActive /*timeActive*/,
                                             kDefaultTimeInactive /*timeInactive*/);
}

TEST_F(TransactionRouterMetricsTest, TimeActiveAndInactiveAdvanceAfterUnknownCommitResult) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(Status(ErrorCodes::HostUnreachable, "dummy"), true /* expectRetries */);

    // timeActive can advance.
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive + Microseconds(100));
    assertTimeInactiveIs(kDefaultTimeInactive);

    // timeInactive can advance.
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive + Microseconds(100));
    assertTimeInactiveIs(kDefaultTimeInactive + Microseconds(100));

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);

    // timeActive can advance.
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive + Microseconds(200));
    assertTimeInactiveIs(kDefaultTimeInactive + Microseconds(100));

    // timeInactive can advance.
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    tickSource()->advance(Microseconds(100));
    assertTimeActiveIs(kDefaultTimeActive + Microseconds(200));
    assertTimeInactiveIs(kDefaultTimeInactive + Microseconds(200));

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kCommit);
    runCommit(kDummyOkRes);

    // The result is known, so neither can advance.
    assertTimeActiveAndInactiveCannotAdvance(kDefaultTimeActive + Microseconds(200) /*timeActive*/,
                                             kDefaultTimeInactive +
                                                 Microseconds(200) /*timeInactive*/);
}

TEST_F(TransactionRouterMetricsTest, NeitherTimeActiveNorInactiveAdvanceAfterAbort) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);

    // Neither can advance.
    assertTimeActiveAndInactiveCannotAdvance(kDefaultTimeActive /*timeActive*/,
                                             kDefaultTimeInactive /*timeInactive*/);
}

TEST_F(TransactionRouterMetricsTest, NeitherTimeActiveNorInactiveAdvanceAfterImplicitAbort) {
    beginTxnWithDefaultTxnNumber();

    setUpDefaultTimeActiveAndInactive();

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    // Neither can advance.
    assertTimeActiveAndInactiveCannotAdvance(kDefaultTimeActive /*timeActive*/,
                                             kDefaultTimeInactive /*timeInactive*/);
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_DefaultTo0) {
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_BeginTxn) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_BeginRecover) {
    beginRecoverCommitWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_Stash) {
    beginRecoverCommitWithDefaultTxnNumber();
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_BeginAfterStash) {
    beginRecoverCommitWithDefaultTxnNumber();
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);

    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_AreNotCumulative) {
    // Test active.
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    // Test inactive.
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 2, TransactionRouter::TransactionActions::kStart);
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 3, TransactionRouter::TransactionActions::kStart);
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_TxnEnds) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_UnknownCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_ImplicitAbortForStashedTxn) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_ImplicitAbortForActiveTxn) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(1L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_BeginAndStashForEndedTxn) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    txnRouter().stash(operationContext(), TransactionRouter::StashReason::kDone);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_CommitEndedTxn) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_ExplicitAbortEndedTxn) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    auto future = launchAsync([&] { txnRouter().abortTransaction(operationContext()); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
    future.default_timed_get();

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCurrent_ImplicitAbortEndedTxn) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());

    auto future = launchAsync(
        [&] { txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), kTxnNumber);
    future.default_timed_get();

    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getCurrentInactive());
}

TEST_F(TransactionRouterTest, RouterMetricsCurrent_ReapForInactiveTxn) {
    auto routerTxnMetrics = RouterTransactionsMetrics::get(operationContext());
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        // Check out a session to create one in the session catalog.
        RouterOperationContextSession routerOpCtxSession(operationContext());

        // Start a transaction on the session.
        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), TxnNumber(5), TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        ASSERT_EQUALS(1L, routerTxnMetrics->getCurrentOpen());
        ASSERT_EQUALS(1L, routerTxnMetrics->getCurrentActive());
        ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentInactive());
    }

    // The router session is out of scope, so the transaction is stashed.
    ASSERT_EQUALS(1L, routerTxnMetrics->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentActive());
    ASSERT_EQUALS(1L, routerTxnMetrics->getCurrentInactive());

    // Mark the session for reap which will also erase it from the catalog.
    auto catalog = SessionCatalog::get(operationContext()->getServiceContext());
    catalog->scanSessionsForReap(*operationContext()->getLogicalSessionId(),
                                 [](ObservableSession& parentSession) {
                                     parentSession.markForReap(
                                         ObservableSession::ReapMode::kNonExclusive);
                                 },
                                 [](ObservableSession& childSession) {});

    // Verify the session was reaped.
    catalog->scanSession(*operationContext()->getLogicalSessionId(), [](const ObservableSession&) {
        FAIL("The callback was called for non-existent session");
    });

    // Verify serverStatus was updated correctly and the reaped transactions were not considered
    // committed or aborted.
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentInactive());
    ASSERT_EQUALS(0L, routerTxnMetrics->getTotalCommitted());
    ASSERT_EQUALS(0L, routerTxnMetrics->getTotalAborted());
}

TEST_F(TransactionRouterTest, RouterMetricsCurrent_ReapForUnstartedTxn) {
    auto routerTxnMetrics = RouterTransactionsMetrics::get(operationContext());
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());

    {
        // Check out a session to create one in the session catalog, but don't start a txn on it.
        RouterOperationContextSession routerOpCtxSession(operationContext());
    }

    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentInactive());

    // Mark the session for reap which will also erase it from the catalog.
    auto catalog = SessionCatalog::get(operationContext()->getServiceContext());
    catalog->scanSessionsForReap(*operationContext()->getLogicalSessionId(),
                                 [](ObservableSession& parentSession) {
                                     parentSession.markForReap(
                                         ObservableSession::ReapMode::kNonExclusive);
                                 },
                                 [](ObservableSession& childSession) {});

    // Verify the session was reaped.
    catalog->scanSession(*operationContext()->getLogicalSessionId(), [](const ObservableSession&) {
        FAIL("The callback was called for non-existent session");
    });

    // Verify serverStatus was not modified and the reaped transactions were not considered
    // committed or aborted.
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentOpen());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentActive());
    ASSERT_EQUALS(0L, routerTxnMetrics->getCurrentInactive());
    ASSERT_EQUALS(0L, routerTxnMetrics->getTotalCommitted());
    ASSERT_EQUALS(0L, routerTxnMetrics->getTotalAborted());
}

// The following three tests verify that the methods that end metrics tracking for a transaction
// can't be called for an unstarted one.

DEATH_TEST_REGEX_F(TransactionRouterMetricsTest,
                   ImplicitlyAbortingUnstartedTxnCrashes,
                   R"#(Invariant failure.*isInitialized\(\))#") {
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);
}

DEATH_TEST_REGEX_F(TransactionRouterMetricsTest,
                   AbortingUnstartedTxnCrashes,
                   R"#(Invariant failure.*isInitialized\(\))#") {
    txnRouter().abortTransaction(operationContext());
}

DEATH_TEST_REGEX_F(TransactionRouterMetricsTest,
                   CommittingUnstartedTxnCrashes,
                   R"#(Invariant failure.*isInitialized\(\))#") {
    txnRouter().commitTransaction(operationContext(), boost::none);
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalStarted_DefaultsTo0) {
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalStarted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalStarted_IncreasedByBeginTxn) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalStarted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalStarted_IncreasedByBeginRecover) {
    beginRecoverCommitWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalStarted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalStarted_IsCumulative) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalStarted());

    operationContext()->setTxnNumber(kTxnNumber + 1);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalStarted());

    // Shouldn't go down when a transaction ends.
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalStarted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_DefaultsTo0) {
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByBeginTxn) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByBeginRecover) {
    beginRecoverCommitWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByFailedCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyErrorRes);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByUnknownCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByExplicitAbort) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_NotIncreasedByImplicitAbort) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest,
       RouterMetricsTotalCommitted_NotIncreasedByAbandonedTransaction) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_IncreasedBySuccessfulCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalCommitted_IsCumulative) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalCommitted());

    operationContext()->setTxnNumber(kTxnNumber + 1);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_DefaultsTo0) {
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_NotIncreasedByBeginTxn) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_NotIncreasedByBeginRecover) {
    beginRecoverCommitWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalCommitted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_NotIncreasedByUnknownCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_NotIncreasedByAbandonedTransaction) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_NotIncreasedBySuccessfulCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_IncreasedByFailedCommit) {
    beginTxnWithDefaultTxnNumber();
    runCommit(kDummyErrorRes);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_IncreasedByExplicitAbort) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_IncreasedByImplicitAbort) {
    beginTxnWithDefaultTxnNumber();
    txnRouter().implicitlyAbortTransaction(operationContext(), kDummyStatus);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalAborted_IsCumulative) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalAborted());

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_THROWS_CODE(txnRouter().abortTransaction(operationContext()),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalAborted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalContactedParticipants) {
    // Starts at 0.
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalContactedParticipants());

    // Only increases when a new participant is created.
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalContactedParticipants());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalContactedParticipants());

    // Only increases for new participants.
    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalContactedParticipants());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalContactedParticipants());

    // Is cumulative across transactions.
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalContactedParticipants());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    ASSERT_EQUALS(3L, routerTxnMetrics()->getTotalContactedParticipants());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalRequestsTargeted) {
    // Starts at 0.
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalRequestsTargeted());

    // Does not increase until a participant is targeted.
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalRequestsTargeted());

    // Increases each time transaction fields are attached.
    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalRequestsTargeted());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalRequestsTargeted());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter().processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    ASSERT_EQUALS(3L, routerTxnMetrics()->getTotalRequestsTargeted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalRequestsTargeted_Recovery) {
    // Total requests targeted is increased by commit recovery.
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalRequestsTargeted());
    beginRecoverCommitWithDefaultTxnNumber();
    txnRouter().setDefaultAtClusterTime(operationContext());

    runRecoverWithTokenCommit(shard1);
    ASSERT_EQUALS(1L, routerTxnMetrics()->getTotalRequestsTargeted());

    // None of the participant stats should be updated since the recovery shard doesn't know the
    // participant list.
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalContactedParticipants());
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalParticipantsAtCommit());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalRequestsTargeted_NetworkErrorRetries) {
    // Total requests targeted does not increase for automatic retries on network errors.
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalRequestsTargeted());

    // The commit will be retried because of the retryable error, but totalRequestsTargeted should
    // only be incremented once per participant. The helper targets one participant, so expect one
    // target for the statement before commit and one for the commit itself, excluding retries.
    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalRequestsTargeted());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsTotalParticipantsAtCommit) {
    // Starts at 0.
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    // Does not increase until commit begins.
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);
    txnRouter().processParticipantResponse(operationContext(), shard2, kOkReadOnlyFalseResponse);
    ASSERT_EQUALS(0L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    // Increases after commit begins, before it ends.
    auto future = beginAndPauseCommit();
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    // Not affected by end of commit.
    expectCoordinateCommitTransaction();
    future.default_timed_get();
    ASSERT_EQUALS(2L, routerTxnMetrics()->getTotalParticipantsAtCommit());

    // Is cumulative across transactions.
    operationContext()->setTxnNumber(kTxnNumber + 1);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    runCommit(kDummyOkRes);
    ASSERT_EQUALS(3L, routerTxnMetrics()->getTotalParticipantsAtCommit());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCommitTypeStatsNotUpdatedOnUnknownResult) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_EQUALS(
        0L,
        routerTxnMetrics()->getCommitTypeStats_forTest(CommitType::kSingleShard).initiated.load());
    ASSERT_EQUALS(
        0L,
        routerTxnMetrics()->getCommitTypeStats_forTest(CommitType::kSingleShard).successful.load());
    ASSERT_EQUALS(0L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());

    runCommit(kDummyRetryableErrorRes, true /* expectRetries */);

    // The result is unknown so only initiated is increased.
    ASSERT_EQUALS(
        1L,
        routerTxnMetrics()->getCommitTypeStats_forTest(CommitType::kSingleShard).initiated.load());
    ASSERT_EQUALS(
        0L,
        routerTxnMetrics()->getCommitTypeStats_forTest(CommitType::kSingleShard).successful.load());
    ASSERT_EQUALS(0L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());
}

TEST_F(TransactionRouterMetricsTest, RouterMetricsCommitTypeStatsSuccessfulDurationMicros) {
    beginTxnWithDefaultTxnNumber();

    // Advancing the clock before beginning commit shouldn't affect commit duration or successful
    // commit duration.
    tickSource()->advance(Microseconds(100));

    auto future = beginAndPauseCommit();

    // The clock hasn't advanced since commit started, so commit duration and successful commit
    // duration should be 0.
    assertCommitDurationIs(Microseconds(0));
    ASSERT_EQUALS(0L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());

    tickSource()->advance(Microseconds(100));

    // Advancing the clock during commit should increase commit duration but not successful commit
    // duration.
    assertCommitDurationIs(Microseconds(100));
    ASSERT_EQUALS(0L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());

    expectCommitTransaction();
    future.default_timed_get();

    // Finishing the commit successfully should now increase successful commit duration but not
    // commit duration.
    assertCommitDurationIs(Microseconds(100));
    ASSERT_EQUALS(100L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());

    // Commit duration and successful commit duration shouldn't change now that commit has finished.
    tickSource()->advance(Microseconds(100));
    assertCommitDurationIs(Microseconds(100));
    ASSERT_EQUALS(100L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());

    // Start a new transaction and verify that successful commit duration is cumulative.
    operationContext()->setTxnNumber(kTxnNumber + 1);
    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber + 1, TransactionRouter::TransactionActions::kStart);
    txnRouter().setDefaultAtClusterTime(operationContext());
    future = beginAndPauseCommit();
    tickSource()->advance(Microseconds(100));
    expectCommitTransaction();
    future.default_timed_get();

    assertCommitDurationIs(Microseconds(100));
    ASSERT_EQUALS(200L,
                  routerTxnMetrics()
                      ->getCommitTypeStats_forTest(CommitType::kSingleShard)
                      .successfulDurationMicros.load());
}

TEST_F(TransactionRouterMetricsTest, ReportResources) {
    // Create client and read concern metadata.
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               "appName",
                                               &builder));

    auto obj = builder.obj();
    auto clientMetadata = ClientMetadata::parse(obj["client"]);
    ClientMetadata::setAndFinalize(operationContext()->getClient(),
                                   std::move(clientMetadata.getValue()));

    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;

    auto clockSource = preciseClockSource();
    auto startTime = Date_t::now();
    clockSource->reset(startTime);

    beginTxnWithDefaultTxnNumber();

    // Verify reported parameters match expectations.
    auto state = txnRouter().reportState(operationContext(), false /* sessionIsActive */);
    auto transactionDocument = state.getObjectField("transaction");

    auto parametersDocument = transactionDocument.getObjectField("parameters");
    ASSERT_EQ(parametersDocument.getField("txnNumber").numberLong(), kTxnNumber);
    ASSERT_EQ(parametersDocument.getField("autocommit").boolean(), false);
    ASSERT_BSONELT_EQ(parametersDocument.getField("readConcern"),
                      readConcernArgs.toBSON().getField("readConcern"));

    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(
        dateFromISOString(transactionDocument.getField("startWallClockTime").valueStringData())
            .getValue(),
        startTime);
    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);
    ASSERT_EQ(transactionDocument.getField("numNonReadOnlyParticipants").numberInt(), 0);
    ASSERT_EQ(transactionDocument.getField("numReadOnlyParticipants").numberInt(), 0);

    ASSERT_EQ(state.getField("host").valueStringData().toString(), getHostNameCachedAndPort());
    ASSERT_EQ(state.getField("desc").valueStringData().toString(), "inactive transaction");
    ASSERT_BSONOBJ_EQ(state.getField("lsid").Obj(), getSessionId().toBSON());
    ASSERT_EQ(state.getField("client").valueStringData().toString(), "");
    ASSERT_EQ(state.getField("connectionId").numberLong(), 0);
    ASSERT_EQ(state.getField("appName").valueStringData().toString(), "appName");
    ASSERT_BSONOBJ_EQ(state.getField("clientMetadata").Obj(), obj.getField("client").Obj());
    ASSERT_EQ(state.getField("active").boolean(), false);
}

TEST_F(TransactionRouterMetricsTest, ReportResourcesWithParticipantList) {
    auto clockSource = preciseClockSource();
    auto startTime = Date_t::now();
    clockSource->reset(startTime);

    beginTxnWithDefaultTxnNumber();
    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard1, kDummyFindCmd);
    txnRouter().attachTxnFieldsIfNeeded(operationContext(), shard2, kDummyFindCmd);

    auto state = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    auto transactionDocument = state.getObjectField("transaction");
    auto parametersDocument = transactionDocument.getObjectField("parameters");

    ASSERT_GTE(transactionDocument.getField("readTimestamp").timestamp(), Timestamp(0, 0));
    ASSERT_EQ(dateFromISOString(transactionDocument.getField("startWallClockTime").String()),
              startTime);

    // Verify participants array matches expected values.

    auto participantComp = [](const BSONElement& a, const BSONElement& b) {
        return a.Obj().getField("name").String() < b.Obj().getField("name").String();
    };

    auto participantArray = transactionDocument.getField("participants").Array();
    ASSERT_EQ(participantArray.size(), 2U);
    std::sort(participantArray.begin(), participantArray.end(), participantComp);

    auto participant1 = participantArray[0].Obj();
    ASSERT_EQ(participant1.getField("name").String(), "shard1");
    ASSERT_EQ(participant1.getField("coordinator").boolean(), true);

    auto participant2 = participantArray[1].Obj();
    ASSERT_EQ(participant2.getField("name").String(), "shard2");
    ASSERT_EQ(participant2.getField("coordinator").boolean(), false);

    txnRouter().processParticipantResponse(operationContext(), shard1, kOkReadOnlyFalseResponse);
    txnRouter().processParticipantResponse(operationContext(), shard2, kOkReadOnlyTrueResponse);

    txnRouter().beginOrContinueTxn(
        operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kContinue);

    // Verify participants array has been updated with proper ReadOnly responses.

    state = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    transactionDocument = state.getObjectField("transaction");
    participantArray = transactionDocument.getField("participants").Array();

    ASSERT_EQ(participantArray.size(), 2U);
    std::sort(participantArray.begin(), participantArray.end(), participantComp);

    participant1 = participantArray[0].Obj();
    ASSERT_EQ(participant1.getField("name").String(), "shard1");
    ASSERT_EQ(participant1.getField("coordinator").boolean(), true);
    ASSERT_EQ(participant1.getField("readOnly").boolean(), false);

    participant2 = participantArray[1].Obj();
    ASSERT_EQ(participant2.getField("name").String(), "shard2");
    ASSERT_EQ(participant2.getField("coordinator").boolean(), false);
    ASSERT_EQ(participant2.getField("readOnly").boolean(), true);

    ASSERT_EQ(transactionDocument.getField("numNonReadOnlyParticipants").numberInt(), 1);
    ASSERT_EQ(transactionDocument.getField("numReadOnlyParticipants").numberInt(), 1);

    ASSERT_GTE(transactionDocument.getField("timeOpenMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeActiveMicros").numberLong(), 0);
    ASSERT_GTE(transactionDocument.getField("timeInactiveMicros").numberLong(), 0);
}

TEST_F(TransactionRouterMetricsTest, ReportResourcesCommit) {
    beginTxnWithDefaultTxnNumber();

    auto clockSource = preciseClockSource();
    auto commitTime = Date_t::now();
    clockSource->reset(commitTime);

    runTwoPhaseCommit();

    // Verify commit is reported as expected.

    auto state = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    auto transactionDocument = state.getObjectField("transaction");
    ASSERT_EQ(dateFromISOString(transactionDocument.getField("commitStartWallClockTime").String()),
              commitTime);
    ASSERT_EQ(transactionDocument.getField("commitType").String(), "twoPhaseCommit");
}

TEST_F(TransactionRouterMetricsTest, ReportResourcesRecoveryCommit) {
    beginSlowRecoverCommitWithDefaultTxnNumber();
    runRecoverWithTokenCommit(boost::none);

    // Verify that the participant list does not exist if the commit type is recovery.

    auto state = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    auto transactionDocument = state.getObjectField("transaction");
    ASSERT_EQ(transactionDocument.hasField("participants"), false);
}

TEST_F(TransactionRouterMetricsTest, ReportResourcesUnstartedTxn) {
    auto state = txnRouter().reportState(operationContext(), true /* sessionIsActive */);
    ASSERT_BSONOBJ_EQ(state, BSONObj());
}

TEST_F(TransactionRouterMetricsTest, IsTrackingOverTrueIfNoTxnStarted) {
    ASSERT(txnRouter().isTrackingOver());
}

TEST_F(TransactionRouterMetricsTest, IsTrackingOverIfTxnCommitted) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_FALSE(txnRouter().isTrackingOver());
    runCommit(kDummyOkRes);
    ASSERT(txnRouter().isTrackingOver());
}

TEST_F(TransactionRouterMetricsTest, IsTrackingOverIfTxnExplcitlyAborted) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_FALSE(txnRouter().isTrackingOver());
    explicitAbortInProgress();
    ASSERT(txnRouter().isTrackingOver());
}

TEST_F(TransactionRouterMetricsTest, IsTrackingOverIfTxnImplicitlyAborted) {
    beginTxnWithDefaultTxnNumber();
    ASSERT_FALSE(txnRouter().isTrackingOver());
    implicitAbortInProgress();
    ASSERT(txnRouter().isTrackingOver());
}

bool doesExistInCatalog(const LogicalSessionId& lsid, SessionCatalog* sessionCatalog) {
    bool existsInCatalog{false};
    sessionCatalog->scanSession(lsid,
                                [&](const ObservableSession& session) { existsInCatalog = true; });
    return existsInCatalog;
}

TEST_F(TransactionRouterTest, EagerlyReapRetryableSessionsUponNewClientTransaction) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runTransactionLeaveOpen(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runTransactionLeaveOpen(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber client transaction and verify the child was erased.

    parentTxnNumber++;
    runTransactionLeaveOpen(parentLsid, parentTxnNumber);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
}

TEST_F(TransactionRouterTest, EagerlyReapRetryableSessionsUponNewRetryableTransaction) {
    auto sessionCatalog = SessionCatalog::get(getServiceContext());
    ASSERT_EQ(sessionCatalog->size(), 0);

    // Add a parent session with one retryable child.

    auto parentLsid = makeLogicalSessionIdForTest();
    auto parentTxnNumber = 0;
    runTransactionLeaveOpen(parentLsid, parentTxnNumber);

    auto retryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runTransactionLeaveOpen(retryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(retryableChildLsid, sessionCatalog));

    // Start a higher txnNumber retryable transaction and verify the child was erased.

    parentTxnNumber++;
    auto higherRetryableChildLsid =
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(parentLsid, parentTxnNumber);
    runTransactionLeaveOpen(higherRetryableChildLsid, 0);

    ASSERT_EQ(sessionCatalog->size(), 1);
    ASSERT(doesExistInCatalog(parentLsid, sessionCatalog));
    ASSERT_FALSE(doesExistInCatalog(retryableChildLsid, sessionCatalog));
    ASSERT(doesExistInCatalog(higherRetryableChildLsid, sessionCatalog));
}

}  // unnamed namespace
}  // namespace mongo
