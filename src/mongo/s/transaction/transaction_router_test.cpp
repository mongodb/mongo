/**
 *    Copyright (C) 2018 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/logical_clock.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

class TransactionRouterTest : public ShardingTestFixture {
protected:
    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

    const ShardId shard1 = ShardId("shard1");
    const HostAndPort hostAndPort1 = HostAndPort("shard1:1234");

    const ShardId shard2 = ShardId("shard2");
    const HostAndPort hostAndPort2 = HostAndPort("shard2:1234");

    void setUp() {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.push_back(std::make_tuple(shard1, hostAndPort1));
        shardInfos.push_back(std::make_tuple(shard2, hostAndPort2));

        ShardingTestFixture::addRemoteShards(shardInfos);

        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));
    }
};

TEST_F(TransactionRouterTest, BasicStartTxn) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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

TEST_F(TransactionRouterTest, ParticipantMustStartTransactionUntilSentCommand) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        ASSERT(participant.mustStartTransaction());
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        ASSERT(participant.mustStartTransaction());
        participant.markAsCommandSent();
        ASSERT_FALSE(participant.mustStartTransaction());
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        ASSERT_FALSE(participant.mustStartTransaction());
    }
}

TEST_F(TransactionRouterTest, BasicStartTxnWithAtClusterTime) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);
    txnRouter.computeAtClusterTimeForOneShard(operationContext(), shard1);

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
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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

TEST_F(TransactionRouterTest, CannotContiueTxnWithoutStarting) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    ASSERT_THROWS_CODE(txnRouter.beginOrContinueTxn(operationContext(), txnNum, false),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionRouterTest, NewParticipantMustAttachTxnAndReadConcern) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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
                                  << "snapshot")
                          << "startTransaction"
                          << true
                          << "autocommit"
                          << false
                          << "txnNumber"
                          << txnNum);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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

TEST_F(TransactionRouterTest, NewParticipantMustAttachTxnAndReadConcernWithAtClusterTime) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);
    txnRouter.computeAtClusterTimeForOneShard(operationContext(), shard1);

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
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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

TEST_F(TransactionRouterTest, StartingNewTxnShouldClearState) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);
    txnRouter.computeAtClusterTimeForOneShard(operationContext(), shard1);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
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
    txnRouter.beginOrContinueTxn(operationContext(), txnNum2, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum2);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }
}

TEST_F(TransactionRouterTest, FirstParticipantIsCoordinator) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    ASSERT_FALSE(txnRouter.getCoordinatorId());


    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        ASSERT(participant.isCoordinator());
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        ASSERT_FALSE(participant.isCoordinator());
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);
    }

    TxnNumber txnNum2{5};
    txnRouter.beginOrContinueTxn(operationContext(), txnNum2, true);

    ASSERT_FALSE(txnRouter.getCoordinatorId());

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        ASSERT(participant.isCoordinator());
        ASSERT(txnRouter.getCoordinatorId());
        ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);
    }
}

TEST_F(TransactionRouterTest, DoesNotAttachTxnNumIfAlreadyThere) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "txnNumber"
                                  << txnNum
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false);

    auto& participant = txnRouter.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"
                                                           << "txnNumber"
                                                           << txnNum));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

DEATH_TEST_F(TransactionRouterTest, CrashesIfCmdHasDifferentTxnNumber, "invariant") {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    auto& participant = txnRouter.getOrCreateParticipant(shard1);
    participant.attachTxnFieldsIfNeeded(BSON("insert"
                                             << "test"
                                             << "txnNumber"
                                             << TxnNumber(10)));
}

TEST_F(TransactionRouterTest, AttachTxnValidatesReadConcernIfAlreadyOnCmd) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);


    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"
                                                               << "readConcern"
                                                               << BSON("level"
                                                                       << "snapshot")));
        ASSERT_BSONOBJ_EQ(BSON("insert"
                               << "test"
                               << "readConcern"
                               << BSON("level"
                                       << "snapshot")
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

TEST_F(TransactionRouterTest, CannotSpecifyReadConcernAfterFirstStatement) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    ASSERT_THROWS_CODE(
        txnRouter.beginOrContinueTxn(operationContext(), txnNum, false /* startTransaction */),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(TransactionRouterTest, UpconvertToSnapshotIfNoReadConcernLevelGiven) {
    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

    TxnNumber txnNum{3};
    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot")
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    auto& participant = txnRouter.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTest, UpconvertToSnapshotIfNoReadConcernLevelButHasAfterClusterTime) {
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(LogicalTime(Timestamp(10, 1)), boost::none);

    TxnNumber txnNum{3};
    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "readConcern"
                                  << BSON("level"
                                          << "snapshot"
                                          // TODO SERVER-36237: afterClusterTime should be replaced
                                          // by an atClusterTime at least as large.
                                          << "afterClusterTime"
                                          << Timestamp(10, 1))
                                  << "startTransaction"
                                  << true
                                  << "coordinator"
                                  << true
                                  << "autocommit"
                                  << false
                                  << "txnNumber"
                                  << txnNum);

    auto& participant = txnRouter.getOrCreateParticipant(shard1);
    auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                           << "test"));
    ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
}

TEST_F(TransactionRouterTest, CannotUpconvertIfLevelOtherThanSnapshotWasGiven) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs(readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter txnRouter({});
        txnRouter.checkOut();
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTest, CannotUpconvertIfLevelOtherThanSnapshotWasGivenWithAfterClusterTime) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(LogicalTime(Timestamp(10, 1)), readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter txnRouter({});
        txnRouter.checkOut();
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTest, CannotUpconvertWithAfterOpTime) {
    auto readConcernLevels = {repl::ReadConcernLevel::kLocalReadConcern,
                              repl::ReadConcernLevel::kMajorityReadConcern,
                              repl::ReadConcernLevel::kLinearizableReadConcern,
                              repl::ReadConcernLevel::kAvailableReadConcern};

    for (auto readConcernLevel : readConcernLevels) {
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::OpTime(Timestamp(10, 1), 2), readConcernLevel);

        TxnNumber txnNum{3};
        TransactionRouter txnRouter({});
        txnRouter.checkOut();
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */),
            DBException,
            ErrorCodes::InvalidOptions);
    }

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::OpTime(Timestamp(10, 1), 2), boost::none);

    {

        TxnNumber txnNum{3};
        TransactionRouter txnRouter({});
        txnRouter.checkOut();
        ASSERT_THROWS_CODE(
            txnRouter.beginOrContinueTxn(operationContext(), txnNum, true /* startTransaction */),
            DBException,
            ErrorCodes::InvalidOptions);
    }
}

TEST_F(TransactionRouterTest, CannotCommitWithoutParticipants) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    ASSERT_THROWS(txnRouter.commitTransaction(operationContext()), AssertionException);
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

TEST_F(TransactionRouterTest, SendCommitDirectlyForSingleParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    ScopedRouterSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, true);
    txnRouter->getOrCreateParticipant(shard1);

    auto future = launchAsync([&] { txnRouter->commitTransaction(operationContext()); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "commitTransaction");

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, SendPrepareAndCoordinateCommitForMultipleParticipants) {
    LogicalSessionId lsid(makeLogicalSessionIdForTest());
    TxnNumber txnNum{3};

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    ScopedRouterSession scopedSession(opCtx);
    auto txnRouter = TransactionRouter::get(opCtx);

    txnRouter->beginOrContinueTxn(opCtx, txnNum, true);
    txnRouter->getOrCreateParticipant(shard1);
    txnRouter->getOrCreateParticipant(shard2);

    auto future = launchAsync([&] { txnRouter->commitTransaction(operationContext()); });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort2, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "prepareTransaction");

        auto coordinator = request.cmdObj["coordinatorId"].str();
        ASSERT_EQ(shard1.toString(), coordinator);

        checkSessionDetails(request.cmdObj, lsid, txnNum, boost::none);

        return BSON("ok" << 1);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(hostAndPort1, request.target);
        ASSERT_EQ("admin", request.dbname);

        auto cmdName = request.cmdObj.firstElement().fieldNameStringData();
        ASSERT_EQ(cmdName, "coordinateCommitTransaction");

        auto participantElements = request.cmdObj["participants"].Array();
        ASSERT_EQ(2u, participantElements.size());

        ASSERT_BSONOBJ_EQ(BSON("shardId" << shard1.toString()), participantElements.front().Obj());
        ASSERT_BSONOBJ_EQ(BSON("shardId" << shard2.toString()), participantElements.back().Obj());

        checkSessionDetails(request.cmdObj, lsid, txnNum, true);

        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionRouterTest, SnapshotErrorsResetAtClusterTime) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    txnRouter.setAtClusterTimeToLatestTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
        participant.markAsCommandSent();
    }

    // Advance the latest time in the logical clock so the retry attempt will pick a later time.
    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTime);

    // Simulate a snapshot error.
    ASSERT(txnRouter.canContinueOnSnapshotError());
    txnRouter.onSnapshotError();

    txnRouter.setAtClusterTimeToLatestTime(operationContext());

    expectedReadConcern = BSON("level"
                               << "snapshot"
                               << "atClusterTime"
                               << laterTime.asTimestamp());

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTest, CannotChangeAtClusterTimeWithoutSnapshotError) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    txnRouter.setAtClusterTimeToLatestTime(operationContext());

    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << kInMemoryLogicalTime.asTimestamp());

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }

    LogicalTime laterTime(Timestamp(1000, 1));
    ASSERT_GT(laterTime, kInMemoryLogicalTime);
    LogicalClock::get(operationContext())->setClusterTimeFromTrustedSource(laterTime);

    txnRouter.setAtClusterTimeToLatestTime(operationContext());

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedReadConcern, newCmd["readConcern"].Obj());
    }
}

TEST_F(TransactionRouterTest, SnapshotErrorsAddAllParticipantsToOrphanedList) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    // Successfully start a transaction on two shards, selecting one as the coordinator.

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        participant.markAsCommandSent();
        ASSERT_FALSE(participant.mustStartTransaction());
    }

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        participant.markAsCommandSent();
        ASSERT_FALSE(participant.mustStartTransaction());
    }

    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard1);

    ASSERT(txnRouter.getOrphanedParticipants().empty());

    // Simulate a snapshot error and an internal retry that only re-targets one of the original two
    // shards.

    ASSERT(txnRouter.canContinueOnSnapshotError());
    txnRouter.onSnapshotError();

    ASSERT_FALSE(txnRouter.getCoordinatorId());
    ASSERT_EQ(txnRouter.getOrphanedParticipants().size(), 2U);

    {
        auto& participant = txnRouter.getOrCreateParticipant(shard2);
        ASSERT(participant.mustStartTransaction());
        participant.markAsCommandSent();
        ASSERT_FALSE(participant.mustStartTransaction());
    }

    // There is a new coordinator and shard1 is still in the orphaned list.
    ASSERT(txnRouter.getCoordinatorId());
    ASSERT_EQ(*txnRouter.getCoordinatorId(), shard2);
    ASSERT_EQ(txnRouter.getOrphanedParticipants().size(), 1U);
    ASSERT_EQ(txnRouter.getOrphanedParticipants().count(shard1), 1U);

    {
        // Shard1 has not started a transaction.
        auto& participant = txnRouter.getOrCreateParticipant(shard1);
        ASSERT(participant.mustStartTransaction());
    }
}

TEST_F(TransactionRouterTest, CanOnlyContinueOnSnapshotErrorOnFirstCommand) {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    ASSERT(txnRouter.canContinueOnSnapshotError());

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, false);
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, false);
    ASSERT_FALSE(txnRouter.canContinueOnSnapshotError());
}

DEATH_TEST_F(TransactionRouterTest, CannotCallOnSnapshotErrorAfterFirstCommand, "invariant") {
    TxnNumber txnNum{3};

    TransactionRouter txnRouter({});
    txnRouter.checkOut();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, true);

    repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();
    txnRouter.beginOrContinueTxn(operationContext(), txnNum, false);

    txnRouter.onSnapshotError();
}

}  // unnamed namespace
}  // namespace mongo
