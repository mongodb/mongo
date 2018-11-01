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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_coordinator_commands_impl.h"
#include "mongo/db/transaction_coordinator_service.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
const std::set<ShardId> kTwoShardIdSet{{"s1"}, {"s2"}};
const std::vector<ShardId> kThreeShardIdList{{"s1"}, {"s2"}, {"s3"}};
const std::set<ShardId> kThreeShardIdSet{{"s1"}, {"s2"}, {"s3"}};
const Timestamp kDummyTimestamp = Timestamp::min();
const Date_t kCommitDeadline = Date_t::max();
const StatusWith<BSONObj> kRetryableError = {ErrorCodes::HostUnreachable, ""};
const StatusWith<BSONObj> kNoSuchTransaction = {ErrorCodes::NoSuchTransaction, ""};
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const StatusWith<BSONObj> kPrepareOk = BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1));

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

class TransactionCoordinatorServiceTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        for (const auto& shardId : kThreeShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();
    }

    void assertCommandSentAndRespondWith(const StringData& commandName,
                                         const StatusWith<BSONObj>& response) {
        onCommand([commandName, response](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(commandName, request.cmdObj.firstElement().fieldNameStringData());
            return response;
        });
    }

    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName, kPrepareOk);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName, kNoSuchTransaction);
    }

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk);
    }

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kOk);
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kRetryableError);
    }

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    LogicalSessionId lsid() {
        return _lsid;
    }

    TxnNumber txnNumber() {
        return _txnNumber;
    }

    /**
     * Goes through the steps to commit a transaction through the coordinator service  for a given
     * lsid and txnNumber. Useful when not explictly testing the commit protocol.
     */
    void commitTransaction(TransactionCoordinatorService& coordinatorService,
                           const LogicalSessionId& lsid,
                           const TxnNumber& txnNumber,
                           const std::set<ShardId>& transactionParticipantShards) {
        auto commitDecisionFuture = coordinatorService.coordinateCommit(
            operationContext(), lsid, txnNumber, transactionParticipantShards);

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertPrepareSentAndRespondWithSuccess();
        }

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertCommitSentAndRespondWithSuccess();
        }

        // Wait for commit to complete.
        commitDecisionFuture.get();
    }

    /**
     * Goes through the steps to abort a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the abort protocol.
     */
    void abortTransaction(TransactionCoordinatorService& coordinatorService,
                          const LogicalSessionId& lsid,
                          const TxnNumber& txnNumber,
                          const std::set<ShardId>& shardIdSet,
                          const ShardId& abortingShard) {
        auto commitDecisionFuture =
            coordinatorService.coordinateCommit(operationContext(), lsid, txnNumber, shardIdSet);

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertPrepareSentAndRespondWithNoSuchTransaction();
        }

        // Abort gets sent to the second participant as soon as the first participant
        // receives a not-okay response to prepare.
        assertAbortSentAndRespondWithSuccess();

        // Wait for abort to complete.
        commitDecisionFuture.get();
    }


private:
    // Override the CatalogClient to make CatalogClient::getAllShards automatically return the
    // expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
    // ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
    // DBClientMock analogous to the NetworkInterfaceMock.
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() : ShardingCatalogClientMock(nullptr) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : kThreeShardIdList) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }
        };

        return stdx::make_unique<StaticCatalogClient>();
    }

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumber _txnNumber{1};
};

/**
 * Fixture that during setUp automatically creates a coordinator service and then creates a
 * coordinator on the service for a default lsid/txnNumber pair.
 */
class TransactionCoordinatorServiceTestSingleTxn : public TransactionCoordinatorServiceTest {
public:
    void setUp() final {
        TransactionCoordinatorServiceTest::setUp();

        _coordinatorService = std::make_unique<TransactionCoordinatorService>();
        _coordinatorService->createCoordinator(
            operationContext(), lsid(), txnNumber(), kCommitDeadline);
    }

    void tearDown() final {
        _coordinatorService.reset();
        TransactionCoordinatorServiceTest::tearDown();
    }

    TransactionCoordinatorService* coordinatorService() {
        return _coordinatorService.get();
    }

private:
    std::unique_ptr<TransactionCoordinatorService> _coordinatorService;
};

}  // namespace

TEST_F(TransactionCoordinatorServiceTest, CreateCoordinatorOnNewSessionSucceeds) {
    TransactionCoordinatorService coordinatorService;
    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber(), kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorForExistingSessionWithPreviouslyCommittedTxnSucceeds) {

    TransactionCoordinatorService coordinatorService;

    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber(), kTwoShardIdSet);

    coordinatorService.createCoordinator(
        operationContext(), lsid(), txnNumber() + 1, kCommitDeadline);
    commitTransaction(coordinatorService, lsid(), txnNumber() + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       RetryingCreateCoordinatorForSameLsidAndTxnNumberSucceeds) {

    TransactionCoordinatorService coordinatorService;

    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);
    // Retry create. This should succeed but not replace the old coordinator.
    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);

    commitTransaction(coordinatorService, lsid(), txnNumber(), kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnNumberThanOngoingCommittingTxnCommitsPreviousTxnAndSucceeds) {

    TransactionCoordinatorService coordinatorService;
    coordinatorService.createCoordinator(operationContext(), lsid(), txnNumber(), kCommitDeadline);

    // Progress the transaction up until the point where it has sent commit and is waiting for
    // commit acks.
    auto oldTxnCommitDecisionFuture = coordinatorService.coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    // Simulate all participants acking prepare/voting to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    // Create a coordinator for a higher transaction number in the same session. This should
    // "tryAbort" on the old coordinator which should NOT abort it since it's already waiting for
    // commit acks.
    coordinatorService.createCoordinator(
        operationContext(), lsid(), txnNumber() + 1, kCommitDeadline);
    auto newTxnCommitDecisionFuture = coordinatorService.coordinateCommit(
        operationContext(), lsid(), txnNumber() + 1, kTwoShardIdSet);

    // Finish committing the old transaction by sending it commit acks from both participants.
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    // The old transaction should now be committed.
    ASSERT_EQ(static_cast<int>(oldTxnCommitDecisionFuture.get()),
              static_cast<int>(TransactionCoordinator::CommitDecision::kCommit));

    // Make sure the newly created one works fine too.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    // commitTransaction(coordinatorService, lsid(), txnNumber() + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnAbort) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    // Simulate a participant voting to abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();

    // Only send abort to the node that voted to commit.
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitWithNoVotesReturnsNotReadyFuture) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    ASSERT_FALSE(commitDecisionFuture.isReady());
    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[0]);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnCommit) {

    auto commitDecisionFuture = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitRecoversCorrectCommitDecisionForTransactionThatAlreadyCommitted) {
    // TODO (SERVER-37440): Implement test when coordinateCommit is made to work correctly on
    // retries.
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitRecoversCorrectCommitDecisionForTransactionThatAlreadyAborted) {
    // TODO (SERVER-37440): Implement test when coordinateCommit is made to work correctly on
    // retries.
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnCommit) {

    auto commitDecisionFuture1 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);
    auto commitDecisionFuture2 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    commitTransaction(*coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnAbort) {

    auto commitDecisionFuture1 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);
    auto commitDecisionFuture2 = coordinatorService()->coordinateCommit(
        operationContext(), lsid(), txnNumber(), kTwoShardIdSet);

    abortTransaction(
        *coordinatorService(), lsid(), txnNumber(), kTwoShardIdSet, kTwoShardIdList[0]);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

}  // namespace mongo
