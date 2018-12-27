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

#include "mongo/db/transaction_coordinator_service.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/write_concern_options.h"
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

const BSONObj kDummyWriteConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                    << "dummy");

const StatusWith<BSONObj> kRetryableError = {ErrorCodes::HostUnreachable, ""};
const StatusWith<BSONObj> kNoSuchTransactionAndWriteConcernError =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction << "writeConcernError"
              << kDummyWriteConcernError);

const StatusWith<BSONObj> kNoSuchTransaction =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction);
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const StatusWith<BSONObj> kOkButWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kDummyWriteConcernError);

const StatusWith<BSONObj> kPrepareOk = BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1));
const StatusWith<BSONObj> kPrepareOkButWriteConcernError =
    BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1) << "writeConcernError"
              << kDummyWriteConcernError);

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

/**
 * Constructs the default options for the thread pool used to run commit.
 */
ThreadPool::Options makeSingleThreadedThreadPoolForTest() {
    ThreadPool::Options options;
    options.poolName = "TransactionCoordinatorService";
    options.minThreads = 0;
    options.maxThreads = 1;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return options;
}

class TransactionCoordinatorServiceTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        // Use a thread pool with a maxThreads=1 so that the unit tests can expect tasks to be run
        // in a deterministic order.
        TransactionCoordinatorService::get(operationContext())
            ->setThreadPoolForTest(
                std::make_unique<ThreadPool>(makeSingleThreadedThreadPoolForTest()));

        ASSERT_OK(ServerParameterSet::getGlobal()
                      ->getMap()
                      .find("logComponentVerbosity")
                      ->second->setFromString("{verbosity: 3}"));

        for (const auto& shardId : kThreeShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }
    }

    void tearDown() override {
        // Join the original thread pool and replace it with a new one, because tasks in the pool
        // may call getGlobalServiceContext, which is set to nullptr before the ServiceContext's
        // destructor (which ordinarily joins the thread pool) is invoked.
        TransactionCoordinatorService::get(operationContext())
            ->setThreadPoolForTest(
                std::make_unique<ThreadPool>(makeSingleThreadedThreadPoolForTest()));
        ShardServerTestFixture::tearDown();
    }

    void assertCommandSentAndRespondWith(const StringData& commandName,
                                         const StatusWith<BSONObj>& response,
                                         const BSONObj& expectedWriteConcern) {
        log() << "Mock storage layer waiting for command " << commandName;
        onCommand([&](const executor::RemoteCommandRequest& request) {
            if (response.isOK()) {
                log() << "Got command " << request.cmdObj.firstElement().fieldNameStringData()
                      << " and responding with " << response.getValue();
            } else {
                log() << "Got command " << request.cmdObj.firstElement().fieldNameStringData()
                      << " and responding with " << response.getStatus();
            }
            ASSERT_EQ(request.cmdObj.firstElement().fieldNameStringData(), commandName);

            ASSERT_BSONOBJ_EQ(
                expectedWriteConcern,
                request.cmdObj.getObjectField(WriteConcernOptions::kWriteConcernField));
            return response;
        });
    }

    // Prepare responses

    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOk,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOkButWriteConcernError,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransaction,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransactionAndWriteConcernError,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    // Abort responses

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk, WriteConcernOptions::Majority);
    }

    void assertAbortSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kOkButWriteConcernError, WriteConcernOptions::Majority);
    }

    void assertAbortSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kNoSuchTransaction, WriteConcernOptions::Majority);
    }

    void assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith("abortTransaction",
                                        kNoSuchTransactionAndWriteConcernError,
                                        WriteConcernOptions::Majority);
    }

    // Commit responses

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kOk, WriteConcernOptions::Majority);
    }

    void assertCommitSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName,
                                        kOkButWriteConcernError,
                                        WriteConcernOptions::Majority);
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kRetryableError, WriteConcernOptions::Majority);
    }

    // Other

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    // TODO (SERVER-38382): Put all these helpers in one separate file and share with
    // transaction_coordinator_test.

    /**
     * Goes through the steps to commit a transaction through the coordinator service  for a given
     * lsid and txnNumber. Useful when not explictly testing the commit protocol.
     */
    void commitTransaction(TransactionCoordinatorService& coordinatorService,
                           const LogicalSessionId& lsid,
                           const TxnNumber& txnNumber,
                           const std::set<ShardId>& transactionParticipantShards) {
        auto commitDecisionFuture = *coordinatorService.coordinateCommit(
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
            *coordinatorService.coordinateCommit(operationContext(), lsid, txnNumber, shardIdSet);

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertPrepareSentAndRespondWithNoSuchTransaction();
        }

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertAbortSentAndRespondWithSuccess();
        }

        // Wait for abort to complete.
        commitDecisionFuture.get();
    }

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
 * Fixture that during setUp automatically creates a coordinator for a default lsid/txnNumber pair.
 */
class TransactionCoordinatorServiceTestSingleTxn : public TransactionCoordinatorServiceTest {
public:
    void setUp() final {
        TransactionCoordinatorServiceTest::setUp();
        TransactionCoordinatorService::get(operationContext())
            ->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);
    }

    TransactionCoordinatorService* coordinatorService() {
        return TransactionCoordinatorService::get(operationContext());
    }
};

}  // namespace

TEST_F(TransactionCoordinatorServiceTest, CreateCoordinatorOnNewSessionSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumber, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorForExistingSessionWithPreviouslyCommittedTxnSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumber, kTwoShardIdSet);

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumber + 1, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumber + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       RetryingCreateCoordinatorForSameLsidAndTxnNumberSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);
    // Retry create. This should succeed but not replace the old coordinator.
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    commitTransaction(*coordinatorService, _lsid, _txnNumber, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnNumberThanOngoingCommittingTxnCommitsPreviousTxnAndSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    // Progress the transaction up until the point where it has sent commit and is waiting for
    // commit acks.
    auto oldTxnCommitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // Simulate all participants acking prepare/voting to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    // Create a coordinator for a higher transaction number in the same session. This should
    // "tryAbort" on the old coordinator which should NOT abort it since it's already waiting for
    // commit acks.
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumber + 1, kCommitDeadline);
    auto newTxnCommitDecisionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber + 1, kTwoShardIdSet);

    // Finish committing the old transaction by sending it commit acks from both participants.
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    // The old transaction should now be committed.
    ASSERT_EQ(static_cast<int>(oldTxnCommitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
    commitTransaction(*coordinatorService, _lsid, _txnNumber + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest, CoordinateCommitReturnsNoneIfNoCoordinatorEverExisted) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    auto commitDecisionFuture =
        coordinatorService->coordinateCommit(operationContext(), _lsid, _txnNumber, kTwoShardIdSet);
    ASSERT(boost::none == commitDecisionFuture);
}

TEST_F(TransactionCoordinatorServiceTest, CoordinateCommitReturnsNoneIfCoordinatorWasRemoved) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumber, kTwoShardIdSet);

    auto commitDecisionFuture =
        coordinatorService->coordinateCommit(operationContext(), _lsid, _txnNumber, kTwoShardIdSet);
    ASSERT(boost::none == commitDecisionFuture);
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitWithSameParticipantListJoinsOngoingCoordinationThatLeadsToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto commitDecisionFuture2 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitWithSameParticipantListJoinsOngoingCoordinationThatLeadsToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();

    auto commitDecisionFuture2 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTest, RecoverCommitJoinsOngoingCoordinationThatLeadsToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto commitDecisionFuture2 =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumber);

    assertPrepareSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTest, RecoverCommitJoinsOngoingCoordinationThatLeadsToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();

    auto commitDecisionFuture2 =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumber);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(
    TransactionCoordinatorServiceTest,
    CreateCoordinatorWithHigherTxnNumberThanExistingButNotYetCommittingTxnCancelsPreviousTxnAndSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    // Create a coordinator for a higher transaction number in the same session. This should
    // cancel commit on the old coordinator.
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumber + 1, kCommitDeadline);
    auto newTxnCommitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber + 1, kTwoShardIdSet);

    // Since this transaction has already been canceled, this should return boost::none.
    auto oldTxnCommitDecisionFuture =
        coordinatorService->coordinateCommit(operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // The old transaction should now be committed.
    ASSERT(oldTxnCommitDecisionFuture == boost::none);

    // Make sure the newly created one works fine too.
    commitTransaction(*coordinatorService, _lsid, _txnNumber + 1, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToPrepare) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // One participant responds with writeConcern error.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();

    // Coordinator retries prepare against participant that responded with writeConcern error until
    // participant responds without writeConcern error.
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccess();

    // Coordinator sends commit.
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    // The transaction should now be committed.
    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // One participant votes to abort.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant responds to abort with success.
    assertAbortSentAndRespondWithSuccess();

    // Coordinator retries abort against other participant until other participant responds without
    // writeConcern error.
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransaction();

    // The transaction should now be aborted.
    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // Both participants vote to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    // One participant responds to commit with success.
    assertCommitSentAndRespondWithSuccess();

    // Coordinator retries commit against other participant until other participant responds without
    // writeConcern error.
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccess();

    // The transaction should now be committed.
    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinatorIsCanceledIfDeadlinePassesAndHasNotReceivedParticipantList) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    const auto deadline = executor()->now() + Milliseconds(1000 * 60 * 10 /* 10 hours */);
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, deadline);

    // Reach the deadline.
    network()->enterNetwork();
    network()->runUntil(deadline);
    network()->exitNetwork();

    // The coordinator should no longer exist.
    ASSERT(boost::none ==
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumber, kTwoShardIdSet));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinatorIsNotCanceledIfDeadlinePassesButHasReceivedParticipantList) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    const auto deadline = executor()->now() + Milliseconds(1000 * 60 * 10 /* 10 hours */);
    coordinatorService->createCoordinator(operationContext(), _lsid, _txnNumber, deadline);

    // Deliver the participant list before the deadline.
    ASSERT(boost::none !=
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumber, kTwoShardIdSet));

    // Reach the deadline.
    network()->enterNetwork();
    network()->runUntil(deadline);
    network()->exitNetwork();

    // The coordinator should still exist.
    ASSERT(boost::none !=
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumber, kTwoShardIdSet));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnAbort) {

    auto commitDecisionFuture = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    // Simulate a participant voting to abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitWithNoVotesReturnsNotReadyFuture) {

    auto commitDecisionFuture = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    ASSERT_FALSE(commitDecisionFuture.isReady());
    // To prevent invariant failure in TransactionCoordinator that all futures have been completed.
    abortTransaction(*coordinatorService(), _lsid, _txnNumber, kTwoShardIdSet, kTwoShardIdList[0]);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnCommit) {

    auto commitDecisionFuture = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnCommit) {

    auto commitDecisionFuture1 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);
    auto commitDecisionFuture2 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    commitTransaction(*coordinatorService(), _lsid, _txnNumber, kTwoShardIdSet);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnAbort) {

    auto commitDecisionFuture1 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);
    auto commitDecisionFuture2 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumber, kTwoShardIdSet);

    abortTransaction(*coordinatorService(), _lsid, _txnNumber, kTwoShardIdSet, kTwoShardIdList[0]);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

}  // namespace mongo
