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
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const BSONObj kOkReadOnlyFalseResponse = BSON("ok" << 1 << "readOnly" << false);
const BSONObj kOkReadOnlyTrueResponse = BSON("ok" << 1 << "readOnly" << true);

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

    const NamespaceString kViewNss = NamespaceString("test.foo");

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        ShardingTestFixture::addRemoteShards({std::make_tuple(shard1, hostAndPort1),
                                              std::make_tuple(shard2, hostAndPort2),
                                              std::make_tuple(shard3, hostAndPort3)});

        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));

        _staleVersionAndSnapshotRetriesBlock = stdx::make_unique<FailPointEnableBlock>(
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

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime"
                                          << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, BasicStartTxnWithAtClusterTime) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime"
                                          << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotContiueTxnWithoutStarting) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        AssertionException,
        ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTestWithDefaultSession, NewParticipantMustAttachTxnAndReadConcern) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime"
                                          << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }

    expectedNewObj = BSON("insert"
                          << "test"
                          << "readConcern"
                          << BSON("level"
                                  << "snapshot"
                                  << "atClusterTime"
                                  << kInMemoryLogicalTime.asTimestamp())
                          << "startTransaction"
                          << true
                          << "autocommit"
                          << false
                          << "txnNumber"
                          << txnNum);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, StartingNewTxnShouldClearState) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("update"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "readConcern"
                               << BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp())
                               << "startTransaction"
                               << true
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }

    TxnNumber txnNum2{5};
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime"
                                          << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum2);

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, FirstParticipantIsCoordinator) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        txnRouter.attachTxnFieldsIfNeeded(shard1, {});
        auto& participant = *txnRouter.getParticipant(shard1);
        ASSERT(participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    {
        txnRouter.attachTxnFieldsIfNeeded(shard2, {});
        auto& participant = *txnRouter.getParticipant(shard2);
        ASSERT(!participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    TxnNumber txnNum2{5};
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        txnRouter.attachTxnFieldsIfNeeded(shard2, {});
        auto& participant = *txnRouter.getParticipant(shard2);
        ASSERT(participant.isCoordinator);
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, DoesNotAttachTxnNumIfAlreadyThere) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "txnNumber"
                                  << txnNum
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          << "atClusterTime"
                                          << kInMemoryLogicalTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                    BSON("insert"
                                                         << "test"
                                                         << "txnNumber"
                                                         << txnNum));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             CrashesIfCmdHasDifferentTxnNumber,
             "invariant") {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1,
                                      BSON("insert"
                                           << "test"
                                           << "txnNumber"
                                           << TxnNumber(10)));
}

TEST_F(TransactionRouterTestWithDefaultSession, AttachTxnValidatesReadConcernIfAlreadyOnCmd) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
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
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp())
                               << "startTransaction"
                               << true
                               << "coordinator"
                               << true
                               << "autocommit"
                               << false
                               << "txnNumber"
                               << txnNum),
                          newCmd);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotSpecifyReadConcernAfterFirstStatement) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(TransactionRouterTestWithDefaultSession, PassesThroughNoReadConcernToParticipants) {
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
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

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("afterClusterTime" << kAfterClusterTime.asTimestamp())
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                    BSON("insert"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTestWithDefaultSession, RejectUnsupportedReadConcernLevels) {
    for (auto readConcernLevel : unsupportedRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(readConcernLevel);

        TxnNumber txnNum{3};
        auto& txnRouter(*TransactionRouter::get(operationContext()));
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
        auto& txnRouter(*TransactionRouter::get(operationContext()));
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
        auto& txnRouter(*TransactionRouter::get(operationContext()));
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(
                operationContext(), txnNum, TransactionRouter::TransactionActions::kStart),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotCommitWithoutParticipantsOrRecoveryToken) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
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

    if (isCoordinator) {
        ASSERT_EQ(*isCoordinator, cmdObj["coordinator"].trueValue());
    } else {
        ASSERT_TRUE(cmdObj["coordinator"].eoo());
    }
}

void checkWriteConcern(const BSONObj& cmdObj, const WriteConcernOptions& expectedWC) {
    auto writeCocernElem = cmdObj["writeConcern"];
    ASSERT_FALSE(writeCocernElem.eoo());
    ASSERT_BSONOBJ_EQ(expectedWC.toBSON(), writeCocernElem.Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForSingleParticipantThatIsReadOnly) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "commitTransaction");

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForSingleParticipantThatDidAWrite) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter.commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "commitTransaction");

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCommitDirectlyForMultipleParticipantsThatAreAllReadOnly) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);
    txnRouter.processParticipantResponse(shard2, kOkReadOnlyTrueResponse);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

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

    future.timed_get(kFutureTimeout);
    ASSERT(expectedHostAndPorts == seenHostAndPorts);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCoordinateCommitForMultipleParticipantsOnlyOneDidAWrite) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);
    txnRouter.processParticipantResponse(shard2, kOkReadOnlyFalseResponse);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

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
            auto shardId = element["shardId"].valuestr();
            ASSERT_EQ(1ull, expectedParticipants.count(shardId));
            expectedParticipants.erase(shardId);
        }

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SendCoordinateCommitForMultipleParticipantsAllDidWrites) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);
    txnRouter.processParticipantResponse(shard2, kOkReadOnlyFalseResponse);

    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

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
            auto shardId = element["shardId"].valuestr();
            ASSERT_EQ(1ull, expectedParticipants.count(shardId));
            expectedParticipants.erase(shardId);
        }

        checkSessionDetails(request.cmdObj, getSessionId(), txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, CommitWithRecoveryTokenWithNoParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(10, WriteConcernOptions::SyncMode::NONE, 0);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(shard1);

    auto future =
        launchAsync([&] { txnRouter->commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_TRUE(participantElements.empty());

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);
        checkWriteConcern(request.cmdObj, writeConcern);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);

    // Sending commit with a recovery token again should cause the router to use the recovery path
    // again.

    future = launchAsync([&] { txnRouter->commitTransaction(operationContext(), recoveryToken); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_TRUE(participantElements.empty());

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);
        checkWriteConcern(request.cmdObj, writeConcern);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, CommitWithEmptyRecoveryToken) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(10, WriteConcernOptions::SyncMode::NONE, 0);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    ASSERT_THROWS_CODE(txnRouter->commitTransaction(operationContext(), recoveryToken),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, CommitWithRecoveryTokenWithUnknownShard) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    WriteConcernOptions writeConcern(10, WriteConcernOptions::SyncMode::NONE, 0);
    opCtx->setWriteConcern(writeConcern);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);
    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kCommit);

    TxnRecoveryToken recoveryToken;
    recoveryToken.setShardId(ShardId("magicShard"));

    auto future =
        launchAsync([&] { txnRouter->commitTransaction(operationContext(), recoveryToken); });

    ShardType shardType;
    shardType.setName(shard1.toString());
    shardType.setHost(hostAndPort1.toString());

    // ShardRegistry will try to perform a reload since it doesn't know about the shard.
    expectGetShards({shardType});

    ASSERT_THROWS_CODE(future.timed_get(kFutureTimeout), DBException, ErrorCodes::ShardNotFound);
}

TEST_F(TransactionRouterTestWithDefaultSession, SnapshotErrorsResetAtClusterTime) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    // Advance the latest time in the logical clock so the retry attempt will pick a later time.
    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTime);

    // Simulate a snapshot error.

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    txnRouter.setDefaultAtClusterTime(operationContext());

    expectedReadConcern = BSON("level"
                               << "snapshot"
                               << "atClusterTime"
                               << laterTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       CannotChangeAtClusterTimeAfterStatementThatSelectedIt) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    // Changing the atClusterTime during the statement that selected it is allowed.

    LogicalTime laterTimeSameStmt(Timestamp(100, 1));
    ASSERT_GT(laterTimeSameStmt, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTimeSameStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    expectedReadConcern = BSON("level"
                               << "snapshot"
                               << "atClusterTime"
                               << laterTimeSameStmt.asTimestamp());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2,
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
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTimeNewStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard3,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, SnapshotErrorsClearsAllParticipants) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Successfully start a transaction on two shards, selecting one as the coordinator.

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate a snapshot error and an internal retry that only re-targets one of the original two
    // shards.

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });
    expectAbortTransactions({hostAndPort1, hostAndPort2}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    txnRouter.setDefaultAtClusterTime(operationContext());

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2, {});
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());

        newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2, {});
        ASSERT_FALSE(newCmd["startTransaction"].trueValue());
    }

    // There is a new coordinator.
    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);

    {
        // Shard1 should also attach startTransaction field again.
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());

        newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
        ASSERT_FALSE(newCmd["startTransaction"].trueValue());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, CannotContinueOnSnapshotErrorAfterFirstCommand) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

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
    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Transaction 1 contacts shard1 and shard2 during the first command, then shard3 in the second
    // command.

    int initialStmtId = 0;
    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});

    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    ShardId shard3("shard3");
    txnRouter.attachTxnFieldsIfNeeded(shard3, {});
    ASSERT_EQ(txnRouter.getParticipant(shard3)->stmtIdCreatedAt, initialStmtId + 1);

    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    // Transaction 2 contacts shard3 and shard2 during the first command, then shard1 in the second
    // command.

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    TxnNumber txnNum2{5};
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard3, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});

    ASSERT_EQ(txnRouter.getParticipant(shard3)->stmtIdCreatedAt, initialStmtId);
    ASSERT_EQ(txnRouter.getParticipant(shard2)->stmtIdCreatedAt, initialStmtId);

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum2, TransactionRouter::TransactionActions::kContinue);

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    ASSERT_EQ(txnRouter.getParticipant(shard1)->stmtIdCreatedAt, initialStmtId + 1);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AllParticipantsAndCoordinatorClearedOnStaleErrorOnFirstCommand) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Start a transaction on two shards, selecting one as the coordinator, but simulate a
    // re-targeting error from at least one of them.

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate stale error and internal retry that only re-targets one of the original shards.

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find"));
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort1, hostAndPort2}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        ASSERT_FALSE(txnRouter.getParticipant(shard2));
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2, {});
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());
    }

    // There is a new coordinator.
    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);

    {
        // Shard1 has not started a transaction.
        ASSERT_FALSE(txnRouter.getParticipant(shard1));
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
        ASSERT_TRUE(newCmd["startTransaction"].trueValue());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession, OnlyNewlyCreatedParticipantsClearedOnStaleError) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // First statement successfully targets one shard, selecing it as the coordinator.

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Start a subsequent statement that targets two new shards and encounters a stale error from at
    // least one of them.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    txnRouter.attachTxnFieldsIfNeeded(shard3, {});

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find"));
    auto future = launchAsync(
        [&] { txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus); });
    expectAbortTransactions({hostAndPort2, hostAndPort3}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    // Shards 2 and 3 must start a transaction, but shard 1 must not.
    ASSERT_FALSE(txnRouter.attachTxnFieldsIfNeeded(shard1, {})["startTransaction"].trueValue());
    ASSERT_TRUE(txnRouter.attachTxnFieldsIfNeeded(shard2, {})["startTransaction"].trueValue());
    ASSERT_TRUE(txnRouter.attachTxnFieldsIfNeeded(shard3, {})["startTransaction"].trueValue());
}

TEST_F(TransactionRouterTestWithDefaultSession,
       RetriesCannotPickNewAtClusterTimeOnStatementAfterSelected) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    // First statement selects an atClusterTime.

    txnRouter.setDefaultAtClusterTime(operationContext());

    // A later statement retries on a stale version error and a view resolution error and cannot
    // change the atClusterTime.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTime);

    ASSERT(txnRouter.canContinueOnStaleShardOrDbError("find"));
    txnRouter.onStaleShardOrDbError(operationContext(), "find", kDummyStatus);
    txnRouter.setDefaultAtClusterTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                    BSON("find"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());

    auto future =
        launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    txnRouter.setDefaultAtClusterTime(operationContext());

    newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                               BSON("find"
                                                    << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession, WritesCanOnlyBeRetriedIfFirstOverallCommand) {
    auto writeCmds = {"insert", "update", "delete", "findAndModify", "findandmodify"};
    auto otherCmds = {"find", "distinct", "aggregate", "killCursors", "getMore"};

    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    for (auto writeCmd : writeCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(writeCmd));
    }

    for (auto cmd : otherCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(cmd));
    }

    // Advance to the next command.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    for (auto writeCmd : writeCmds) {
        ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError(writeCmd));
    }

    for (auto cmd : otherCmds) {
        ASSERT(txnRouter.canContinueOnStaleShardOrDbError(cmd));
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
    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());

    ASSERT_THROWS_CODE(
        txnRouter->abortTransaction(opCtx), DBException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, AbortForSingleParticipant) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());
    txnRouter->attachTxnFieldsIfNeeded(shard1, {});

    auto future = launchAsync([&] { return txnRouter->abortTransaction(operationContext()); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);

        return kOkReadOnlyFalseResponse;
    });

    auto response = future.timed_get(kFutureTimeout);
    ASSERT_FALSE(response.empty());
}

TEST_F(TransactionRouterTest, AbortForMultipleParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());
    txnRouter->attachTxnFieldsIfNeeded(shard1, {});
    txnRouter->attachTxnFieldsIfNeeded(shard2, {});
    txnRouter->processParticipantResponse(shard1, kOkReadOnlyFalseResponse);
    txnRouter->processParticipantResponse(shard2, kOkReadOnlyFalseResponse);

    auto future = launchAsync([&] { return txnRouter->abortTransaction(operationContext()); });

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

    auto response = future.timed_get(kFutureTimeout);
    ASSERT_FALSE(response.empty());
}

TEST_F(TransactionRouterTestWithDefaultSession, OnViewResolutionErrorClearsAllNewParticipants) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // One shard is targeted by the first statement.
    auto firstShardCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    ASSERT_TRUE(firstShardCmd["startTransaction"].trueValue());

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    // Simulate a view resolution error on the first client statement that leads to a retry which
    // targets the same shard.

    auto future =
        launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    // The only participant was the coordinator, so it should have been reset.
    ASSERT_FALSE(txnRouter.getCoordinatorId());

    // The first shard is targeted by the retry and should have to start a transaction again.
    firstShardCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    ASSERT_TRUE(firstShardCmd["startTransaction"].trueValue());

    // Advance to a later client statement that targets a new shard.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    auto secondShardCmd = txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    ASSERT_TRUE(secondShardCmd["startTransaction"].trueValue());

    // Simulate a view resolution error.

    future = launchAsync([&] { txnRouter.onViewResolutionError(operationContext(), kViewNss); });
    expectAbortTransactions({hostAndPort2}, getSessionId(), txnNum);
    future.timed_get(kFutureTimeout);

    // Only the new participant shard was reset.
    firstShardCmd = txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    ASSERT_FALSE(firstShardCmd["startTransaction"].trueValue());
    secondShardCmd = txnRouter.attachTxnFieldsIfNeeded(shard2, {});
    ASSERT_TRUE(secondShardCmd["startTransaction"].trueValue());
}

TEST_F(TransactionRouterTest, ImplicitAbortIsNoopWithNoParticipants) {
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());

    // Should not throw.
    txnRouter->implicitlyAbortTransaction(opCtx, kDummyStatus);
}

TEST_F(TransactionRouterTest, ImplicitAbortForSingleParticipant) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());
    txnRouter->attachTxnFieldsIfNeeded(shard1, {});

    auto future = launchAsync(
        [&] { return txnRouter->implicitlyAbortTransaction(operationContext(), kDummyStatus); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);

        return kOkReadOnlyFalseResponse;
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, ImplicitAbortForMultipleParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());
    txnRouter->attachTxnFieldsIfNeeded(shard1, {});
    txnRouter->attachTxnFieldsIfNeeded(shard2, {});

    auto future = launchAsync(
        [&] { return txnRouter->implicitlyAbortTransaction(operationContext(), kDummyStatus); });

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

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, ImplicitAbortIgnoresErrors) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    RouterOperationContextSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter->setDefaultAtClusterTime(operationContext());
    txnRouter->attachTxnFieldsIfNeeded(shard1, {});

    auto future = launchAsync(
        [&] { return txnRouter->implicitlyAbortTransaction(operationContext(), kDummyStatus); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "abortTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);

        return BSON("ok" << 0);
    });

    // Shouldn't throw.
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       CannotContinueOnSnapshotOrStaleVersionErrorsWithoutFailpoint) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    disableRouterRetriesFailPoint();

    // Cannot retry on snapshot errors on the first statement.
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());

    // Cannot retry on stale shard or db version errors for read or write commands.
    ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError("find"));
    ASSERT_FALSE(txnRouter.canContinueOnStaleShardOrDbError("insert"));

    // Can still continue on view resolution errors.
    txnRouter.onViewResolutionError(operationContext(), kViewNss);  // Should not throw.
}

TEST_F(TransactionRouterTestWithDefaultSession, ContinuingTransactionPlacesItsReadConcernOnOpCtx) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
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

    auto& txnRouter(*TransactionRouter::get(operationContext()));
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
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                    BSON("insert"
                                                         << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());

    // The next statement cannot change the atClusterTime.

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kContinue);

    LogicalTime laterTimeSameStmt(Timestamp(100, 1));
    ASSERT_GT(laterTimeSameStmt, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTimeSameStmt);

    txnRouter.setDefaultAtClusterTime(operationContext());

    newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2,
                                               BSON("insert"
                                                    << "test"));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
}

TEST_F(TransactionRouterTestWithDefaultSession, NonSnapshotReadConcernHasNoAtClusterTime) {
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(rcIt.second);

        auto& txnRouter(*TransactionRouter::get(operationContext()));
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);

        // No atClusterTime is placed on the router by default.
        ASSERT_FALSE(txnRouter.getAtClusterTime());

        // Can't compute and set an atClusterTime.
        txnRouter.setDefaultAtClusterTime(operationContext());
        ASSERT_FALSE(txnRouter.getAtClusterTime());

        // Can't continue on snapshot errors.
        ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       SupportedNonSnapshotReadConcernLevelsArePassedThrough) {
    TxnNumber txnNum{3};
    for (auto rcIt : supportedNonSnapshotRCLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(rcIt.second);

        auto& txnRouter(*TransactionRouter::get(operationContext()));
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        const BSONObj expectedRC = BSON("level" << rcIt.first);
        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(expectedRC, newCmd["readConcern"].Obj());

        // Only attached on first command to a participant.
        newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                   BSON("insert"
                                                        << "test"));
        ASSERT(newCmd["readConcern"].eoo());

        // Attached for new participants after the first one.
        newCmd = txnRouter.attachTxnFieldsIfNeeded(shard2,
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

        auto& txnRouter(*TransactionRouter::get(operationContext()));
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());

        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
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

        auto& txnRouter(*TransactionRouter::get(operationContext()));
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum++, TransactionRouter::TransactionActions::kStart);

        // Call setDefaultAtClusterTime to simulate real command execution.
        txnRouter.setDefaultAtClusterTime(operationContext());

        auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                        BSON("insert"
                                                             << "test"));
        ASSERT_BSONOBJ_EQ(BSON("level" << rcIt.first << "afterOpTime" << opTime),
                          newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesIgnoresNoSuchTransaction) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // NoSuchTransaction is ignored when it is the top-level error code.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });

    auto noSuchTransactionError = [&] {
        BSONObjBuilder bob;
        CommandHelpers::appendCommandStatusNoThrow(bob,
                                                   Status(ErrorCodes::NoSuchTransaction, "dummy"));
        return bob.obj();
    }();

    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum, noSuchTransactionError);

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesUsesIdempotentRetryPolicy) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // Retryable top-level error.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    ASSERT(txnRouter.canContinueOnSnapshotError());
    auto future = launchAsync([&] { txnRouter.onSnapshotError(operationContext(), kDummyStatus); });

    auto retryableError = [&] {
        BSONObjBuilder bob;
        CommandHelpers::appendCommandStatusNoThrow(
            bob, Status(ErrorCodes::InterruptedDueToStepDown, "dummy"));
        return bob.obj();
    }();

    // The first abort fails with a retryable error, which should be retried.
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum, retryableError);
    expectAbortTransactions({hostAndPort1}, getSessionId(), txnNum);

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       AbortBetweenStatementRetriesFailsWithNoSuchTransactionOnUnexpectedErrors) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);

    //
    // Non-retryable top-level error.
    //

    txnRouter.setDefaultAtClusterTime(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

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

    future.timed_get(kFutureTimeout);
}

DEATH_TEST_F(TransactionRouterTestWithDefaultSession,
             ProcessParticipantResponseInvariantsIfParticipantDoesNotExist,
             "Participant should exist if processing participant response") {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    // Add some participants to the list.
    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.attachTxnFieldsIfNeeded(shard2, {});

    // Simulate response from some participant not in the list.
    txnRouter.processParticipantResponse(shard3, kOkReadOnlyTrueResponse);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseDoesNotUpdateParticipantIfResponseStatusIsNotOk) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.processParticipantResponse(shard1, BSON("ok" << 0));
    ASSERT(TransactionRouter::Participant::ReadOnly::kUnset ==
           txnRouter.getParticipant(shard1)->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseMarksParticipantAsReadOnlyIfResponseSaysReadOnlyTrue) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);

    const auto participant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);

    // Further responses with readOnly: true do not change the participant's readOnly field.

    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);

    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseMarksParticipantAsNotReadOnlyIfFirstResponseSaysReadOnlyFalse) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);

    const auto participant = txnRouter.getParticipant(shard1);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    // Further responses with readOnly: false do not change the participant's readOnly field.

    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);
    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);
}

TEST_F(
    TransactionRouterTestWithDefaultSession,
    ProcessParticipantResponseUpdatesParticipantToReadOnlyFalseIfLaterResponseSaysReadOnlyFalse) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    // First response says readOnly: true.
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse);

    const auto participant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kReadOnly == participant->readOnly);

    // Later response says readOnly: false.
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);

    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseThrowsIfParticipantClaimsToChangeFromReadOnlyFalseToReadOnlyTrue) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    // First response says readOnly: false.
    txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse);

    const auto participant = txnRouter.getParticipant(shard1);

    ASSERT(TransactionRouter::Participant::ReadOnly::kNotReadOnly == participant->readOnly);

    // Later response says readOnly: true.
    ASSERT_THROWS_CODE(txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse),
                       AssertionException,
                       51113);
}

TEST_F(TransactionRouterTestWithDefaultSession,
       ProcessParticipantResponseThrowsIfParticipantReturnsErrorThenSuccessOnLaterStatement) {
    TxnNumber txnNum{3};

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    txnRouter.beginOrContinueTxn(
        operationContext(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    txnRouter.attachTxnFieldsIfNeeded(shard1, {});

    // First response is an error.
    txnRouter.processParticipantResponse(shard1, BSON("ok" << 0));
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
    ASSERT_THROWS_CODE(txnRouter.processParticipantResponse(shard1, kOkReadOnlyTrueResponse),
                       AssertionException,
                       51112);
    ASSERT_THROWS_CODE(txnRouter.processParticipantResponse(shard1, kOkReadOnlyFalseResponse),
                       AssertionException,
                       51112);
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

        auto& txnRouter(*TransactionRouter::get(operationContext()));
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }
};

TEST_F(TransactionRouterTestWithDefaultSessionAndStartedSnapshot, AddAtClusterTimeNormal) {
    auto& txnRouter(*TransactionRouter::get(operationContext()));
    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
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

    auto& txnRouter(*TransactionRouter::get(operationContext()));
    auto newCmd = txnRouter.attachTxnFieldsIfNeeded(shard1,
                                                    BSON("aggregate"
                                                         << "testColl"
                                                         << "readConcern"
                                                         << BSON("level"
                                                                 << "snapshot"
                                                                 << "afterClusterTime"
                                                                 << existingAfterClusterTime)));

    ASSERT_BSONOBJ_EQ(rcLatestInMemoryAtClusterTime, newCmd["readConcern"].Obj());
}

}  // unnamed namespace
}  // namespace mongo
