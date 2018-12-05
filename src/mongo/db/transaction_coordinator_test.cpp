
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

#include "mongo/db/transaction_coordinator.h"
#include "mongo/db/transaction_coordinator_util.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/session_catalog.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"

namespace mongo {

using namespace txn;

namespace {

const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
const std::set<ShardId> kTwoShardIdSet{{"s1"}, {"s2"}};
const std::vector<ShardId> kThreeShardIdList{{"s1"}, {"s2"}, {"s3"}};
const std::set<ShardId> kThreeShardIdSet{{"s1"}, {"s2"}, {"s3"}};
const Timestamp kDummyTimestamp = Timestamp::min();
const Date_t kCommitDeadline = Date_t::max();
const StatusWith<BSONObj> kRetryableError = {ErrorCodes::HostUnreachable, ""};
const StatusWith<BSONObj> kNoSuchTransaction =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction);
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const Timestamp kDummyPrepareTimestamp = Timestamp(1, 1);

StatusWith<BSONObj> makePrepareOkResponse(const Timestamp& timestamp) {
    return BSON("ok" << 1 << "prepareTimestamp" << timestamp);
}

const StatusWith<BSONObj> kPrepareOk = makePrepareOkResponse(kDummyPrepareTimestamp);

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

/**
 * Constructs the default options for the thread pool used to run commit.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
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


class TransactionCoordinatorTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

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
        _pool = std::make_unique<ThreadPool>(makeDefaultThreadPoolOptions());
        _pool->startup();
        _executor = Grid::get(getGlobalServiceContext())->getExecutorPool()->getFixedExecutor();
        _coordinator =
            std::make_shared<TransactionCoordinator>(_executor, _pool.get(), _lsid, _txnNumber);
    }

    void tearDown() override {
        // Prevent invariant in destructor from firing.
        _coordinator->setState_forTest(TransactionCoordinator::CoordinatorState::kDone);
        _coordinator.reset();
        _pool->shutdown();
        _pool.reset();

        ShardServerTestFixture::tearDown();
    }

    void assertCommandSentAndRespondWith(const StringData& commandName,
                                         const StatusWith<BSONObj>& response,
                                         boost::optional<BSONObj> expectedWriteConcern) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElement().fieldNameStringData(), commandName);
            if (expectedWriteConcern) {
                ASSERT_BSONOBJ_EQ(
                    *expectedWriteConcern,
                    request.cmdObj.getObjectField(WriteConcernOptions::kWriteConcernField));
            }
            return response;
        });
    }

    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOk,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }


    void assertPrepareSentAndRespondWithSuccess(const Timestamp& timestamp) {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        makePrepareOkResponse(timestamp),
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransaction,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kRetryableError,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk, boost::none);
    }

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kOk, boost::none);
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kRetryableError, boost::none);
    }

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    std::shared_ptr<TransactionCoordinator> coordinator() {
        return _coordinator;
    }

    ThreadPool* pool() {
        return _pool.get();
    }

    executor::TaskExecutor* executor() {
        return _executor;
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
    void commitTransaction(const std::set<ShardId>& transactionParticipantShards) {
        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertPrepareSentAndRespondWithSuccess(kDummyPrepareTimestamp);
        }

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertCommitSentAndRespondWithSuccess();
        }
    }

    /**
     * Goes through the steps to abort a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the abort protocol.
     */
    void abortTransaction(const std::set<ShardId>& shardIdSet, const ShardId& abortingShard) {
        //    auto commitDecisionFuture =
        //        coordinatorService.coordinateCommit(operationContext(), lsid, txnNumber,
        //        shardIdSet);

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertPrepareSentAndRespondWithNoSuchTransaction();
        }

        // Abort gets sent to the second participant as soon as the first participant
        // receives a not-okay response to prepare.
        assertAbortSentAndRespondWithSuccess();

        // Wait for abort to complete.
        //    commitDecisionFuture.get();
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

    std::unique_ptr<ThreadPool> _pool;
    executor::TaskExecutor* _executor;
    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumber _txnNumber{1};
    std::shared_ptr<TransactionCoordinator> _coordinator;
};

auto makeDummyPrepareCommand(const LogicalSessionId& lsid, const TxnNumber& txnNumber) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::InternalMajorityNoSnapshot));


    return prepareObj;
}

}  // namespace

TEST_F(TransactionCoordinatorTest, SendDecisionToParticipantShardReturnsOnImmediateSuccess) {

    Future<void> future = sendDecisionToParticipantShard(
        executor(), pool(), kTwoShardIdList[0], makeDummyPrepareCommand(lsid(), txnNumber()));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorTest,
       SendDecisionToParticipantShardReturnsSuccessAfterOneFailureAndThenSuccess) {
    Future<void> future = sendDecisionToParticipantShard(
        executor(), pool(), kTwoShardIdList[0], makeDummyPrepareCommand(lsid(), txnNumber()));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorTest,
       SendDecisionToParticipantShardReturnsSuccessAfterSeveralFailuresAndThenSuccess) {
    Future<void> future = sendDecisionToParticipantShard(
        executor(), pool(), kTwoShardIdList[0], makeDummyPrepareCommand(lsid(), txnNumber()));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorTest, SendDecisionToParticipantShardInterpretsVoteToAbortAsSuccess) {
    Future<void> future = sendDecisionToParticipantShard(
        executor(), pool(), kTwoShardIdList[0], makeDummyPrepareCommand(lsid(), txnNumber()));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithNoSuchTransaction();
}

TEST_F(TransactionCoordinatorTest, SendPrepareToShardReturnsCommitDecisionOnOkResponse) {
    Future<PrepareResponse> future =
        sendPrepareToShard(executor(),
                           pool(),
                           makeDummyPrepareCommand(lsid(), txnNumber()),
                           kTwoShardIdList[0],
                           coordinator());

    std::move(future).getAsync([](StatusWith<PrepareResponse> swResponse) {
        ASSERT_OK(swResponse.getStatus());
        auto response = swResponse.getValue();
        ASSERT(response.vote == PrepareVote::kCommit);
        ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
    });

    // Simulate a participant voting to commit.
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareToShardReturnsCommitDecisionOnRetryableErrorThenOkResponse) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);
    Future<PrepareResponse> future =
        sendPrepareToShard(executor(),
                           pool(),
                           makeDummyPrepareCommand(lsid(), txnNumber()),
                           kTwoShardIdList[0],
                           coordinator());

    std::move(future).getAsync([](StatusWith<PrepareResponse> swResponse) {
        ASSERT_OK(swResponse.getStatus());
        auto response = swResponse.getValue();
        ASSERT(response.vote == PrepareVote::kCommit);
        ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
    });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(
    TransactionCoordinatorTest,
    SendPrepareToShardStopsRetryingAfterRetryableErrorAndReturnsNoneIfCoordinatorStateIsNotPrepare) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);
    Future<PrepareResponse> future =
        sendPrepareToShard(executor(),
                           pool(),
                           makeDummyPrepareCommand(lsid(), txnNumber()),
                           kTwoShardIdList[0],
                           coordinator());

    auto resultFuture = std::move(future).then([](PrepareResponse response) {
        ASSERT(response.vote == boost::none);
        ASSERT(response.prepareTimestamp == boost::none);
    });

    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kAborting);
    assertPrepareSentAndRespondWithRetryableError();
    resultFuture.get();
}

TEST_F(TransactionCoordinatorTest, SendPrepareToShardReturnsAbortDecisionOnVoteAbortResponse) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepareToShard(executor(),
                           pool(),
                           makeDummyPrepareCommand(lsid(), txnNumber()),
                           kTwoShardIdList[0],
                           coordinator())
            .then([&](PrepareResponse response) {
                ASSERT(response.vote == PrepareVote::kAbort);
                ASSERT(response.prepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    t.join();
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareToShardReturnsAbortDecisionOnRetryableErrorThenVoteAbortResponse) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepareToShard(executor(),
                           pool(),
                           makeDummyPrepareCommand(lsid(), txnNumber()),
                           kTwoShardIdList[0],
                           coordinator())
            .then([&](PrepareResponse response) {
                ASSERT(response.vote == PrepareVote::kAbort);
                ASSERT(response.prepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    t.join();
}

TEST_F(TransactionCoordinatorTest, CollectReturnsInitValueWhenInputIsEmptyVector) {
    std::vector<Future<int>> futures;
    auto resultFuture = collect(std::move(futures), 0, [](int& result, const int& next) {
        result = 20;
        return ShouldStopIteration::kNo;
    });
    ASSERT_EQ(resultFuture.get(), 0);
}

TEST_F(TransactionCoordinatorTest, CollectReturnsOnlyResultWhenOnlyOneFuture) {
    std::vector<Future<int>> futures;
    auto pf = makePromiseFuture<int>();
    futures.push_back(std::move(pf.future));
    auto resultFuture = collect(std::move(futures), 0, [](int& result, const int& next) {
        result = next;
        return ShouldStopIteration::kNo;
    });
    pf.promise.emplaceValue(3);

    ASSERT_EQ(resultFuture.get(), 3);
}

TEST_F(TransactionCoordinatorTest, CollectReturnsCombinedResultWithSeveralInputFutures) {

    std::vector<Future<int>> futures;
    std::vector<Promise<int>> promises;
    std::vector<int> futureValues;
    for (int i = 0; i < 5; ++i) {
        auto pf = makePromiseFuture<int>();
        futures.push_back(std::move(pf.future));
        promises.push_back(std::move(pf.promise));
        futureValues.push_back(i);
    }

    // Sum all of the inputs.
    auto resultFuture = collect(std::move(futures), 0, [](int& result, const int& next) {
        result += next;
        return ShouldStopIteration::kNo;
    });

    for (size_t i = 0; i < promises.size(); ++i) {
        promises[i].emplaceValue(futureValues[i]);
    }

    // Result should be the sum of all the values emplaced into the promises.
    ASSERT_EQ(resultFuture.get(), std::accumulate(futureValues.begin(), futureValues.end(), 0));
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesAbortAndSecondVotesCommit) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kAbort);
                ASSERT(response.maxPrepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();
    t.join();
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesCommitAndSecondVotesAbort) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kAbort);
                ASSERT(response.maxPrepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    t.join();
}

TEST_F(TransactionCoordinatorTest, SendPrepareReturnsAbortDecisionWhenBothParticipantsVoteAbort) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kAbort);
                ASSERT(response.maxPrepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    t.join();
}

TEST_F(
    TransactionCoordinatorTest,
    SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesAbortEvenThoughSecondResponseHasntBeenReceived) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kAbort);
                ASSERT(response.maxPrepareTimestamp == boost::none);
            })
            .get();
    });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    t.join();  // Should be able to return after the first participant responds.
    assertPrepareSentAndRespondWithNoSuchTransaction();
}

TEST_F(TransactionCoordinatorTest, SendPrepareReturnsCommitDecisionWhenBothParticipantsVoteCommit) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kCommit);
                ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
            })
            .get();
    });

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    t.join();  // Should be able to return after the first participant responds.
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenFirstParticipantHasMax) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kCommit);
                ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
            })
            .get();
    });

    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    t.join();  // Should be able to return after the first participant responds.
}

TEST_F(TransactionCoordinatorTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenLastParticipantHasMax) {
    coordinator()->setState_forTest(TransactionCoordinator::CoordinatorState::kPreparing);

    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    stdx::thread t([&]() {
        sendPrepare(coordinator(), executor(), pool(), kTwoShardIdList, lsid(), txnNumber())
            .then([&](PrepareVoteConsensus response) {
                ASSERT(response.decision == TransactionCoordinator::CommitDecision::kCommit);
                ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
            })
            .get();
    });

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    t.join();  // Should be able to return after the first participant responds.
}

TEST_F(TransactionCoordinatorTest, RunCommitReturnsCorrectCommitDecisionOnAbort) {
    auto commitDecisionFuture = coordinator()->runCommit(kTwoShardIdList);

    // Simulate a participant voting to abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorTest, RunCommitReturnsCorrectCommitDecisionOnCommit) {
    auto commitDecisionFuture = coordinator()->runCommit(kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnAbortAfterNetworkRetriesOneParticipantAborts) {
    auto commitDecisionFuture = coordinator()->runCommit(kTwoShardIdList);

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant votes commit.
    assertPrepareSentAndRespondWithSuccess();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnAbortAfterNetworkRetriesBothParticipantsAbort) {
    auto commitDecisionFuture = coordinator()->runCommit(kTwoShardIdList);

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant votes abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kAbort));
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnCommitAfterNetworkRetries) {
    auto commitDecisionFuture = coordinator()->runCommit(kTwoShardIdList);

    // One participant votes commit after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    // One participant votes commit after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision),
              static_cast<int>(TransactionCoordinator::CommitDecision::kCommit));
}

}  // namespace mongo
