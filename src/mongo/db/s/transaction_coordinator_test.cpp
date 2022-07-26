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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_metrics_observer.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using CoordinatorCommitDecision = txn::CoordinatorCommitDecision;
using PrepareResponse = txn::PrepareResponse;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

const Hours kLongFutureTimeout(8);

const StatusWith<BSONObj> kNoSuchTransaction =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction << "errmsg"
              << "No such transaction exists");
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const Timestamp kDummyPrepareTimestamp = Timestamp(1, 1);

StatusWith<BSONObj> makePrepareOkResponse(const Timestamp& timestamp) {
    return BSON("ok" << 1 << "prepareTimestamp" << timestamp);
}

const StatusWith<BSONObj> kPrepareOk = makePrepareOkResponse(kDummyPrepareTimestamp);
const StatusWith<BSONObj> kPrepareOkNoTimestamp = BSON("ok" << 1);
const StatusWith<BSONObj> kTxnRetryCounterTooOld =
    BSON("ok" << 0 << "code" << ErrorCodes::TxnRetryCounterTooOld << "errmsg"
              << "txnRetryCounter is too old"
              << "txnRetryCounter" << 1);

/**
 * Searches for a client matching the name and mark the operation context as killed.
 */
void killClientOpCtx(ServiceContext* service, const std::string& clientName) {
    for (int retries = 0; retries < 20; retries++) {
        for (ServiceContext::LockedClientsCursor cursor(service); auto client = cursor.next();) {
            invariant(client);

            stdx::lock_guard lk(*client);
            if (client->desc() == clientName) {
                if (auto opCtx = client->getOperationContext()) {
                    opCtx->getServiceContext()->killOperation(
                        lk, opCtx, ErrorCodes::InterruptedAtShutdown);
                    return;
                }
            }
        }

        sleepmillis(50);
    }

    LOGV2_ERROR(
        22462, "Timed out trying to find and kill client opCtx", "clientName"_attr = clientName);
    ASSERT_FALSE(true);
}

class TransactionCoordinatorTestBase : public TransactionCoordinatorTestFixture {
protected:
    explicit TransactionCoordinatorTestBase(Options options = {})
        : TransactionCoordinatorTestFixture(std::move(options)) {}

    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kPrepareOk, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithSuccess(const Timestamp& timestamp) {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        makePrepareOkResponse(timestamp),
                                        WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kNoSuchTransaction, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kRetryableError, WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
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

    void waitUntilCoordinatorDocIsPresent() {
        DBDirectClient dbClient(operationContext());
        while (dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace, BSONObj{})
                   .isEmpty())
            ;
    }

    /**
     * Precondition: A coordinator document exists with or without a decision.
     */
    void waitUntilCoordinatorDocHasDecision() {
        DBDirectClient dbClient(operationContext());
        TransactionCoordinatorDocument doc;
        do {
            doc = TransactionCoordinatorDocument::parse(
                IDLParserContext("dummy"),
                dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace, BSONObj{}));
        } while (!doc.getDecision());
    }

    void waitUntilNoCoordinatorDocIsPresent() {
        DBDirectClient dbClient(operationContext());
        while (!dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace, BSONObj{})
                    .isEmpty())
            ;
    }

    void waitUntilMessageSent() {
        while (true) {
            executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
            if (network()->hasReadyRequests()) {
                return;
            }
        }
    }

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumberAndRetryCounter _txnNumberAndRetryCounter{1, 1};
};

class TransactionCoordinatorDriverTest : public TransactionCoordinatorTestBase {
protected:
    void setUp() override {
        TransactionCoordinatorTestBase::setUp();
        _aws.emplace(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorTestBase::tearDown();
    }

    boost::optional<txn::AsyncWorkScheduler> _aws;
};

auto makeDummyPrepareCommand(const LogicalSessionId& lsid,
                             const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName(NamespaceString::kAdminDb);
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumberAndRetryCounter.getTxnNumber()
                    << "txnRetryCounter" << *txnNumberAndRetryCounter.getTxnRetryCounter()
                    << "autocommit" << false << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));

    return prepareObj;
}

TEST_F(TransactionCoordinatorDriverTest, SendDecisionToParticipantShardReturnsOnImmediateSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterOneFailureAndThenSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithRetryableError();
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterSeveralFailuresAndThenSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardInterpretsVoteToAbortAsSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithNoSuchTransaction();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardCanBeInterruptedAndReturnsError) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithRetryableError();
    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    advanceClockAndExecuteScheduledTasks();

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
}

TEST_F(TransactionCoordinatorDriverTest, SendPrepareToShardReturnsCommitDecisionOnOkResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kCommit);
    ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsCommitDecisionOnRetryableErrorThenOkResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithRetryableError();
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kCommit);
    ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardCanBeInterruptedAndReturnsNoDecisionIfNotServiceShutdown) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithRetryableError();
    const auto shutdownStatus =
        Status{ErrorCodes::TransactionCoordinatorReachedAbortDecision, "Retry interrupted"};
    aws.shutdown(shutdownStatus);
    advanceClockAndExecuteScheduledTasks();

    auto response = future.get();
    ASSERT(response.vote == boost::none);
    ASSERT(response.prepareTimestamp == boost::none);
    ASSERT_EQ(response.abortReason->code(), ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardCanBeInterruptedAndThrowsExceptionIfServiceShutdown) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithRetryableError();
    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Service shutting down"});
    advanceClockAndExecuteScheduledTasks();

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnVoteAbortResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kAbort);
    ASSERT(response.prepareTimestamp == boost::none);
    ASSERT(response.abortReason);
    ASSERT_EQ(ErrorCodes::NoSuchTransaction, response.abortReason->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnRetryableErrorThenVoteAbortResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future =
        txn::sendPrepareToShard(getServiceContext(),
                                aws,
                                _lsid,
                                _txnNumberAndRetryCounter,
                                kTwoShardIdList[0],
                                makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kAbort);
    ASSERT(response.prepareTimestamp == boost::none);
    ASSERT(response.abortReason);
    ASSERT_EQ(ErrorCodes::NoSuchTransaction, response.abortReason->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesAbortAndSecondVotesCommit) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kPrepareOk; }});

    auto decision = future.get().decision();

    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
    ASSERT(decision.getAbortStatus());
    ASSERT_EQ(ErrorCodes::NoSuchTransaction, decision.getAbortStatus()->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesCommitAndSecondVotesAbort) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
    ASSERT(decision.getAbortStatus());
    ASSERT_EQ(ErrorCodes::NoSuchTransaction, decision.getAbortStatus()->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenBothParticipantsVoteAbort) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; }});

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
    ASSERT(decision.getAbortStatus());
    ASSERT_EQ(ErrorCodes::NoSuchTransaction, decision.getAbortStatus()->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWhenBothParticipantsVoteCommit) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT(!decision.getAbortStatus());
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenFirstParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT(!decision.getAbortStatus());
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenLastParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT(!decision.getAbortStatus());
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenNoPreparedTimestampIsReturned) {
    const auto timestamp = Timestamp(1, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(timestamp);
    assertCommandSentAndRespondWith(
        PrepareTransaction::kCommandName, kPrepareOkNoTimestamp, WriteConcernOptions::Majority);

    auto decision = future.get().decision();

    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
    ASSERT(decision.getAbortStatus());
    ASSERT_EQ(50993, int(decision.getAbortStatus()->code()));
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsErrorWhenOneShardReturnsReadConcernMajorityNotEnabled) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(Timestamp(100, 1));
    assertCommandSentAndRespondWith(
        PrepareTransaction::kCommandName,
        BSON("ok" << 0 << "code" << ErrorCodes::ReadConcernMajorityNotEnabled << "errmsg"
                  << "Read concern majority not enabled"),
        WriteConcernOptions::Majority);

    auto decision = future.get().decision();

    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
    ASSERT(decision.getAbortStatus());
    ASSERT_EQ(ErrorCodes::ReadConcernMajorityNotEnabled, decision.getAbortStatus()->code());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareAndDecisionAttachTxnRetryCounterIfFeatureFlagIsEnabled) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto prepareFuture = txn::sendPrepare(getServiceContext(),
                                          aws,
                                          _lsid,
                                          _txnNumberAndRetryCounter,
                                          APIParameters(),
                                          kOneShardIdList);
    onCommands({[&](const executor::RemoteCommandRequest& request) {
        ASSERT_TRUE(request.cmdObj.hasField("txnRetryCounter"));
        ASSERT_EQUALS(request.cmdObj.getIntField("txnRetryCounter"),
                      *_txnNumberAndRetryCounter.getTxnRetryCounter());
        return kNoSuchTransaction;
    }});
    prepareFuture.get();

    auto commitFuture = txn::sendCommit(getServiceContext(),
                                        aws,
                                        _lsid,
                                        _txnNumberAndRetryCounter,
                                        APIParameters(),
                                        kOneShardIdList,
                                        {});
    onCommands({[&](const executor::RemoteCommandRequest& request) {
        ASSERT_TRUE(request.cmdObj.hasField("txnRetryCounter"));
        ASSERT_EQUALS(request.cmdObj.getIntField("txnRetryCounter"),
                      *_txnNumberAndRetryCounter.getTxnRetryCounter());
        return kNoSuchTransaction;
    }});
    commitFuture.get();

    auto abortFuture = txn::sendAbort(getServiceContext(),
                                      aws,
                                      _lsid,
                                      _txnNumberAndRetryCounter,
                                      APIParameters(),
                                      kOneShardIdList);
    onCommands({[&](const executor::RemoteCommandRequest& request) {
        ASSERT_TRUE(request.cmdObj.hasField("txnRetryCounter"));
        ASSERT_EQUALS(request.cmdObj.getIntField("txnRetryCounter"),
                      *_txnNumberAndRetryCounter.getTxnRetryCounter());
        return kNoSuchTransaction;
    }});
    abortFuture.get();
}

class TransactionCoordinatorDriverPersistenceTest : public TransactionCoordinatorDriverTest {
protected:
    void setUp() override {
        TransactionCoordinatorDriverTest::setUp();
        _aws.emplace(getServiceContext());
    }

    void tearDown() override {
        _aws.reset();
        TransactionCoordinatorDriverTest::tearDown();
    }

    static void assertDocumentMatches(
        TransactionCoordinatorDocument doc,
        LogicalSessionId expectedLsid,
        TxnNumberAndRetryCounter expectedTxnNumberAndRetryCounter,
        std::vector<ShardId> expectedParticipants,
        boost::optional<txn::CommitDecision> expectedDecision = boost::none,
        boost::optional<Timestamp> expectedCommitTimestamp = boost::none) {
        ASSERT(doc.getId().getSessionId());
        ASSERT_EQUALS(*doc.getId().getSessionId(), expectedLsid);
        ASSERT(doc.getId().getTxnNumber());
        ASSERT_EQUALS(*doc.getId().getTxnNumber(), expectedTxnNumberAndRetryCounter.getTxnNumber());
        ASSERT(doc.getId().getTxnRetryCounter());
        ASSERT_EQUALS(*doc.getId().getTxnRetryCounter(),
                      *expectedTxnNumberAndRetryCounter.getTxnRetryCounter());

        ASSERT(doc.getParticipants() == expectedParticipants);

        auto decision = doc.getDecision();
        if (expectedDecision) {
            ASSERT(*expectedDecision == decision->getDecision());
        } else {
            ASSERT(!decision);
        }

        if (expectedCommitTimestamp) {
            ASSERT(decision->getCommitTimestamp());
            ASSERT_EQUALS(*expectedCommitTimestamp, *decision->getCommitTimestamp());
        } else if (decision) {
            ASSERT(!decision->getCommitTimestamp());
        }
    }

    void persistParticipantListExpectSuccess(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                             const std::vector<ShardId>& participants) {
        txn::persistParticipantsList(*_aws, lsid, txnNumberAndRetryCounter, participants).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        assertDocumentMatches(allCoordinatorDocs[0], lsid, txnNumberAndRetryCounter, participants);
    }

    void persistDecisionExpectSuccess(OperationContext* opCtx,
                                      LogicalSessionId lsid,
                                      TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                      const std::vector<ShardId>& participants,
                                      const boost::optional<Timestamp>& commitTimestamp) {
        txn::persistDecision(*_aws,
                             lsid,
                             txnNumberAndRetryCounter,
                             participants,
                             [&] {
                                 txn::CoordinatorCommitDecision decision;
                                 if (commitTimestamp) {
                                     decision.setDecision(txn::CommitDecision::kCommit);
                                     decision.setCommitTimestamp(commitTimestamp);
                                 } else {
                                     decision.setDecision(txn::CommitDecision::kAbort);
                                     decision.setAbortStatus(Status(ErrorCodes::NoSuchTransaction,
                                                                    "Test abort status"));
                                 }
                                 return decision;
                             }())
            .get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        if (commitTimestamp) {
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumberAndRetryCounter,
                                  participants,
                                  txn::CommitDecision::kCommit,
                                  *commitTimestamp);
        } else {
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumberAndRetryCounter,
                                  participants,
                                  txn::CommitDecision::kAbort);
        }
    }

    void deleteCoordinatorDocExpectSuccess(OperationContext* opCtx,
                                           LogicalSessionId lsid,
                                           TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
        txn::deleteCoordinatorDoc(*_aws, lsid, txnNumberAndRetryCounter).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(0));
    }

    const std::vector<ShardId> _participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};

    const Timestamp _commitTimestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};

    boost::optional<txn::AsyncWorkScheduler> _aws;
};

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenNoDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenMatchingDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenDocumentWithConflictingParticipantListExistsFailsToPersistList) {
    auto opCtx = operationContext();
    std::vector<ShardId> participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};
    persistParticipantListExpectSuccess(opCtx, _lsid, _txnNumberAndRetryCounter, participants);

    // We should retry until shutdown. The original participants should be persisted.

    std::vector<ShardId> smallerParticipantList{ShardId("shard0001"), ShardId("shard0002")};
    auto future = txn::persistParticipantsList(
        *_aws, _lsid, _txnNumberAndRetryCounter, smallerParticipantList);

    _aws->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    advanceClockAndExecuteScheduledTasks();
    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, _txnNumberAndRetryCounter, participants);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListForMultipleTransactionsOnSameSession) {
    for (int i = 1; i <= 3; i++) {
        txn::persistParticipantsList(
            *_aws, _lsid, {i, *_txnNumberAndRetryCounter.getTxnRetryCounter()}, _participants)
            .get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListForOneTransactionMultipleTxnRetryCountersOnSameSession) {
    const auto numRetries = 3;
    for (int i = 1; i <= numRetries; i++) {
        txn::persistParticipantsList(
            *_aws, _lsid, {_txnNumberAndRetryCounter.getTxnNumber(), i}, _participants)
            .get();
        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, PersistParticipantListForMultipleSessions) {
    for (int i = 1; i <= 3; i++) {
        auto lsid = makeLogicalSessionIdForTest();
        txn::persistParticipantsList(*_aws, lsid, _txnNumberAndRetryCounter, _participants).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 boost::none /* abort */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 boost::none /* abort */);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 boost::none /* abort */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenNoDocumentForTransactionExistsCanBeInterruptedAndReturnsError) {
    Future<repl::OpTime> future;

    {
        FailPointEnableBlock failpoint("hangBeforeWritingDecision");
        future = txn::persistDecision(*_aws, _lsid, _txnNumberAndRetryCounter, _participants, [&] {
            txn::CoordinatorCommitDecision decision(txn::CommitDecision::kCommit);
            decision.setCommitTimestamp(_commitTimestamp);
            return decision;
        }());
        failpoint->waitForTimesEntered(failpoint.initialTimesEntered() + 1);
        _aws->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    }

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 _commitTimestamp /* commit */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 _commitTimestamp /* commit */);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 _commitTimestamp /* commit */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, DeleteCoordinatorDocWhenNoDocumentExistsFails) {
    ASSERT_THROWS_CODE(txn::deleteCoordinatorDoc(*_aws, _lsid, _txnNumberAndRetryCounter).get(),
                       AssertionException,
                       51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithDifferentTxnNumberFails) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    ASSERT_THROWS_CODE(txn::deleteCoordinatorDoc(*_aws,
                                                 _lsid,
                                                 {_txnNumberAndRetryCounter.getTxnNumber() + 1,
                                                  *_txnNumberAndRetryCounter.getTxnRetryCounter()})
                           .get(),
                       AssertionException,
                       51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithDifferentTxnRetryCounterFails) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    ASSERT_THROWS_CODE(
        txn::deleteCoordinatorDoc(*_aws,
                                  _lsid,
                                  {_txnNumberAndRetryCounter.getTxnNumber(),
                                   *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1})
            .get(),
        AssertionException,
        51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithoutDecisionFails) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    ASSERT_THROWS_CODE(txn::deleteCoordinatorDoc(*_aws, _lsid, _txnNumberAndRetryCounter).get(),
                       AssertionException,
                       51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 boost::none /* abort */);
    deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumberAndRetryCounter);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds) {
    persistParticipantListExpectSuccess(
        operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
    persistDecisionExpectSuccess(operationContext(),
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 _participants,
                                 _commitTimestamp /* commit */);
    deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumberAndRetryCounter);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       MultipleTxnNumbersCommitDecisionsPersistedAndDeleteOneSuccessfullyRemovesCorrectDecision) {
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter1{
        _txnNumberAndRetryCounter.getTxnNumber(), *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter2{
        _txnNumberAndRetryCounter.getTxnNumber() + 1,
        *_txnNumberAndRetryCounter.getTxnRetryCounter()};

    // Insert coordinator documents for two transactions.
    txn::persistParticipantsList(*_aws, _lsid, txnNumberAndRetryCounter1, _participants).get();
    txn::persistParticipantsList(*_aws, _lsid, txnNumberAndRetryCounter2, _participants).get();

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(2));

    // Delete the document for the first transaction and check that only the second transaction's
    // document still exists.
    txn::persistDecision(*_aws,
                         _lsid,
                         txnNumberAndRetryCounter1,
                         _participants,
                         [&] {
                             txn::CoordinatorCommitDecision decision(txn::CommitDecision::kAbort);
                             decision.setAbortStatus(
                                 Status(ErrorCodes::NoSuchTransaction, "Test abort error"));
                             return decision;
                         }())
        .get();
    txn::deleteCoordinatorDoc(*_aws, _lsid, txnNumberAndRetryCounter1).get();

    allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, txnNumberAndRetryCounter2, _participants);
}

TEST_F(
    TransactionCoordinatorDriverPersistenceTest,
    MultipleTxnRetryCountersCommitDecisionsPersistedAndDeleteOneSuccessfullyRemovesCorrectDecision) {
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter1{
        _txnNumberAndRetryCounter.getTxnNumber(), *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    const TxnNumberAndRetryCounter txnNumberAndRetryCounter2{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};

    // Insert coordinator documents for two transactions.
    txn::persistParticipantsList(*_aws, _lsid, txnNumberAndRetryCounter1, _participants).get();
    txn::persistParticipantsList(*_aws, _lsid, txnNumberAndRetryCounter2, _participants).get();

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(2));

    // Delete the document for the first transaction and check that only the second transaction's
    // document still exists.
    txn::persistDecision(*_aws,
                         _lsid,
                         txnNumberAndRetryCounter1,
                         _participants,
                         [&] {
                             txn::CoordinatorCommitDecision decision(txn::CommitDecision::kAbort);
                             decision.setAbortStatus(
                                 Status(ErrorCodes::NoSuchTransaction, "Test abort error"));
                             return decision;
                         }())
        .get();
    txn::deleteCoordinatorDoc(*_aws, _lsid, txnNumberAndRetryCounter1).get();

    allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, txnNumberAndRetryCounter2, _participants);
}

using TransactionCoordinatorTest = TransactionCoordinatorTestBase;

TEST_F(TransactionCoordinatorTest, RunCommitProducesCommitDecisionOnTwoCommitResponses) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnAbortAndCommitResponses) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kPrepareOk; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnCommitAndAbortResponsesNoSuchTransaction) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnCommitAndAbortResponsesTxnRetryCounterTooOld) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    onCommands(
        {[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
         [&](const executor::RemoteCommandRequest& request) { return kTxnRetryCounterTooOld; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::TxnRetryCounterTooOld);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::TxnRetryCounterTooOld);
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnSingleAbortResponseOnly) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    assertPrepareSentAndRespondWithNoSuchTransaction();
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneCommitResponseAndOneAbortResponseAfterRetry) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    // One participant votes commit and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) { return kRetryableError; }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the scheduled retry executes

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithNoSuchTransaction();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneAbortResponseAndOneRetryableAbortResponse) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    // One participant votes abort and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kRetryableError; }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesCommitDecisionOnCommitAfterMultipleNetworkRetries) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

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
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesReadConcernMajorityNotEnabledIfEitherShardReturnsIt) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    // One participant votes commit and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) {
                    return BSON("ok" << 0 << "code" << ErrorCodes::ReadConcernMajorityNotEnabled
                                     << "errmsg"
                                     << "Read concern majority not enabled");
                }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::ReadConcernMajorityNotEnabled);
    ASSERT_THROWS_CODE(coordinator.onCompletion().get(),
                       AssertionException,
                       ErrorCodes::ReadConcernMajorityNotEnabled);
}

class TransactionCoordinatorMetricsTest : public TransactionCoordinatorTestBase {
protected:
    TransactionCoordinatorMetricsTest()
        : TransactionCoordinatorTestBase(
              Options{}.useMockClock(true).useMockTickSource<Microseconds>(true)) {}

    void setUp() override {
        tickSource()->reset(1);

        TransactionCoordinatorTestBase::setUp();
    }

    ServerTransactionCoordinatorsMetrics* metrics() {
        return ServerTransactionCoordinatorsMetrics::get(getServiceContext());
    }

    ClockSourceMock* clockSource() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    TickSourceMock<Microseconds>* tickSource() {
        return dynamic_cast<TickSourceMock<Microseconds>*>(getServiceContext()->getTickSource());
    }

    struct Stats {
        // Start times
        boost::optional<Date_t> createTime;
        boost::optional<Date_t> writingParticipantListStartTime;
        boost::optional<Date_t> waitingForVotesStartTime;
        boost::optional<Date_t> writingDecisionStartTime;
        boost::optional<Date_t> waitingForDecisionAcksStartTime;
        boost::optional<Date_t> deletingCoordinatorDocStartTime;
        boost::optional<Date_t> endTime;

        // Durations
        boost::optional<Microseconds> totalDuration;
        boost::optional<Microseconds> twoPhaseCommitDuration;
        boost::optional<Microseconds> writingParticipantListDuration;
        boost::optional<Microseconds> waitingForVotesDuration;
        boost::optional<Microseconds> writingDecisionDuration;
        boost::optional<Microseconds> waitingForDecisionAcksDuration;
        boost::optional<Microseconds> deletingCoordinatorDocDuration;
    };

    void checkStats(const SingleTransactionCoordinatorStats& stats, const Stats& expected) {

        // Start times

        if (expected.createTime) {
            ASSERT_EQ(*expected.createTime, stats.getCreateTime());
        }

        if (expected.writingParticipantListStartTime) {
            ASSERT(*expected.writingParticipantListStartTime ==
                   stats.getWritingParticipantListStartTime());
        }

        if (expected.waitingForVotesStartTime) {
            ASSERT(*expected.waitingForVotesStartTime == stats.getWaitingForVotesStartTime());
        }

        if (expected.writingDecisionStartTime) {
            ASSERT(*expected.writingDecisionStartTime == stats.getWritingDecisionStartTime());
        }

        if (expected.waitingForDecisionAcksStartTime) {
            ASSERT(*expected.waitingForDecisionAcksStartTime ==
                   stats.getWaitingForDecisionAcksStartTime());
        }

        if (expected.deletingCoordinatorDocStartTime) {
            ASSERT(*expected.deletingCoordinatorDocStartTime ==
                   stats.getDeletingCoordinatorDocStartTime());
        }

        if (expected.endTime) {
            ASSERT(*expected.endTime == stats.getEndTime());
        }

        // Durations

        if (expected.totalDuration) {
            ASSERT_EQ(*expected.totalDuration,
                      stats.getDurationSinceCreation(tickSource(), tickSource()->getTicks()));
        }

        if (expected.twoPhaseCommitDuration) {
            ASSERT_EQ(*expected.twoPhaseCommitDuration,
                      stats.getTwoPhaseCommitDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.writingParticipantListDuration) {
            ASSERT_EQ(
                *expected.writingParticipantListDuration,
                stats.getWritingParticipantListDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.waitingForVotesDuration) {
            ASSERT_EQ(*expected.waitingForVotesDuration,
                      stats.getWaitingForVotesDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.writingDecisionDuration) {
            ASSERT_EQ(*expected.writingDecisionDuration,
                      stats.getWritingDecisionDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.waitingForDecisionAcksDuration) {
            ASSERT_EQ(
                *expected.waitingForDecisionAcksDuration,
                stats.getWaitingForDecisionAcksDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.deletingCoordinatorDocDuration) {
            ASSERT_EQ(
                *expected.deletingCoordinatorDocDuration,
                stats.getDeletingCoordinatorDocDuration(tickSource(), tickSource()->getTicks()));
        }
    }

    struct Metrics {
        // Totals
        std::int64_t totalCreated{0};
        std::int64_t totalStartedTwoPhaseCommit{0};
        std::int64_t totalAbortedTwoPhaseCommit{0};
        std::int64_t totalCommittedTwoPhaseCommit{0};

        // Current in steps
        std::int64_t currentWritingParticipantList{0};
        std::int64_t currentWaitingForVotes{0};
        std::int64_t currentWritingDecision{0};
        std::int64_t currentWaitingForDecisionAcks{0};
        std::int64_t currentDeletingCoordinatorDoc{0};
    };

    void checkMetrics(const Metrics& expectedMetrics) {
        // Totals
        ASSERT_EQ(expectedMetrics.totalCreated, metrics()->getTotalCreated());
        ASSERT_EQ(expectedMetrics.totalStartedTwoPhaseCommit,
                  metrics()->getTotalStartedTwoPhaseCommit());
        ASSERT_EQ(expectedMetrics.totalAbortedTwoPhaseCommit,
                  metrics()->getTotalAbortedTwoPhaseCommit());
        ASSERT_EQ(expectedMetrics.totalCommittedTwoPhaseCommit,
                  metrics()->getTotalSuccessfulTwoPhaseCommit());

        // Current in steps
        ASSERT_EQ(expectedMetrics.currentWritingParticipantList,
                  metrics()->getCurrentWritingParticipantList());
        ASSERT_EQ(expectedMetrics.currentWaitingForVotes, metrics()->getCurrentWaitingForVotes());
        ASSERT_EQ(expectedMetrics.currentWritingDecision, metrics()->getCurrentWritingDecision());
        ASSERT_EQ(expectedMetrics.currentWaitingForDecisionAcks,
                  metrics()->getCurrentWaitingForDecisionAcks());
        ASSERT_EQ(expectedMetrics.currentDeletingCoordinatorDoc,
                  metrics()->getCurrentDeletingCoordinatorDoc());
    }

    void checkServerStatus() {
        TransactionCoordinatorsSSS tcsss;
        BSONElement dummy;
        const auto serverStatusSection = tcsss.generateSection(operationContext(), dummy);
        ASSERT_EQ(metrics()->getTotalCreated(), serverStatusSection["totalCreated"].Long());
        ASSERT_EQ(metrics()->getTotalStartedTwoPhaseCommit(),
                  serverStatusSection["totalStartedTwoPhaseCommit"].Long());
        ASSERT_EQ(
            metrics()->getCurrentWritingParticipantList(),
            serverStatusSection.getObjectField("currentInSteps")["writingParticipantList"].Long());
        ASSERT_EQ(metrics()->getCurrentWaitingForVotes(),
                  serverStatusSection.getObjectField("currentInSteps")["waitingForVotes"].Long());
        ASSERT_EQ(metrics()->getCurrentWritingDecision(),
                  serverStatusSection.getObjectField("currentInSteps")["writingDecision"].Long());
        ASSERT_EQ(
            metrics()->getCurrentWaitingForDecisionAcks(),
            serverStatusSection.getObjectField("currentInSteps")["waitingForDecisionAcks"].Long());
        ASSERT_EQ(
            metrics()->getCurrentDeletingCoordinatorDoc(),
            serverStatusSection.getObjectField("currentInSteps")["deletingCoordinatorDoc"].Long());
    }

    static void assertClientReportStateFields(BSONObj doc, std::string appName, int connectionId) {
        ASSERT_EQ(StringData(doc.getStringField("appName")), appName);
        ASSERT_EQ(doc.getIntField("connectionId"), connectionId);

        auto expectedDriverName = std::string("DriverName").insert(0, appName);
        auto expectedDriverVersion = std::string("DriverVersion").insert(0, appName);
        auto expectedOsType = std::string("OsType").insert(0, appName);
        auto expectedOsName = std::string("OsName").insert(0, appName);
        auto expectedOsArch = std::string("OsArchitecture").insert(0, appName);
        auto expectedOsVersion = std::string("OsVersion").insert(0, appName);

        ASSERT_TRUE(doc.hasField("clientMetadata"));
        auto driver = doc.getObjectField("clientMetadata").getObjectField("driver");
        ASSERT_EQ(StringData(driver.getStringField("name")), expectedDriverName);
        ASSERT_EQ(StringData(driver.getStringField("version")), expectedDriverVersion);
        auto os = doc.getObjectField("clientMetadata").getObjectField("os");
        ASSERT_EQ(StringData(os.getStringField("type")), expectedOsType);
        ASSERT_EQ(StringData(os.getStringField("name")), expectedOsName);
        ASSERT_EQ(StringData(os.getStringField("architecture")), expectedOsArch);
        ASSERT_EQ(StringData(os.getStringField("version")), expectedOsVersion);
    }

    Date_t advanceClockSourceAndReturnNewNow() {
        const auto newNow = Date_t::now();
        clockSource()->reset(newNow);
        return newNow;
    }

    void runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines() {
        startCapturingLogMessages();

        TransactionCoordinator coordinator(
            operationContext(),
            _lsid,
            _txnNumberAndRetryCounter,
            std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
            Date_t::max());

        coordinator.runCommit(operationContext(), kTwoShardIdList);

        assertPrepareSentAndRespondWithSuccess();
        assertPrepareSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();

        coordinator.onCompletion().get();
        stopCapturingLogMessages();
    }
};

TEST_F(TransactionCoordinatorMetricsTest, SingleCoordinatorStatsSimpleTwoPhaseCommit) {
    Stats expectedStats;
    TransactionCoordinatorMetricsObserver coordinatorObserver;
    const auto& stats = coordinatorObserver.getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);

    // Stats are updated on onCreate.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    coordinatorObserver.onCreate(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes the total duration to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWritingParticipantList.

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.twoPhaseCommitDuration = Microseconds(0);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    coordinatorObserver.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes the total duration, two-phase commit duration, and duration writing
    // participant list to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWaitingForVotes.

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.waitingForVotesDuration = Microseconds(0);
    coordinatorObserver.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // waiting for votes to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWritingDecision.

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.writingDecisionDuration = Microseconds(0);
    coordinatorObserver.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // writing decision to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWaitingForDecisionAcks.

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    coordinatorObserver.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // waiting for decision acks to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartDeletingCoordinatorDoc.

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    coordinatorObserver.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // deleting the coordinator doc to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onEnd.
    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    coordinatorObserver.onEnd(metrics(),
                              tickSource(),
                              clockSource()->now(),
                              TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                              CoordinatorCommitDecision(txn::CommitDecision::kCommit));
    checkStats(stats, expectedStats);

    // Once onEnd has been called, advancing the time does not cause any duration to increase.
    tickSource()->advance(Microseconds(100));
    checkStats(stats, expectedStats);
}

TEST_F(TransactionCoordinatorMetricsTest, ServerWideMetricsSimpleTwoPhaseCommit) {
    TransactionCoordinatorMetricsObserver coordinatorObserver;
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Metrics are updated on onCreate.
    expectedMetrics.totalCreated++;
    coordinatorObserver.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWritingParticipantList.
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWaitingForVotes.
    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWritingDecision.
    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWaitingForDecisionAcks.
    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartDeletingCoordinatorDoc.
    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onEnd.
    expectedMetrics.currentDeletingCoordinatorDoc--;
    expectedMetrics.totalAbortedTwoPhaseCommit++;
    coordinatorObserver.onEnd(metrics(),
                              tickSource(),
                              clockSource()->now(),
                              TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                              CoordinatorCommitDecision(txn::CommitDecision::kAbort));
    checkMetrics(expectedMetrics);
}

TEST_F(TransactionCoordinatorMetricsTest, ServerWideMetricsSimpleTwoPhaseCommitTwoCoordinators) {
    TransactionCoordinatorMetricsObserver coordinatorObserver1;
    TransactionCoordinatorMetricsObserver coordinatorObserver2;
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Increment each coordinator one step at a time.

    expectedMetrics.totalCreated++;
    coordinatorObserver1.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalCreated++;
    coordinatorObserver2.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver1.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver2.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver1.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver2.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver1.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver2.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver1.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver2.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver1.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver2.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentDeletingCoordinatorDoc--;
    expectedMetrics.totalAbortedTwoPhaseCommit++;
    coordinatorObserver1.onEnd(metrics(),
                               tickSource(),
                               clockSource()->now(),
                               TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                               CoordinatorCommitDecision(txn::CommitDecision::kAbort));
    checkMetrics(expectedMetrics);

    expectedMetrics.currentDeletingCoordinatorDoc--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;
    coordinatorObserver2.onEnd(metrics(),
                               tickSource(),
                               clockSource()->now(),
                               TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                               CoordinatorCommitDecision(txn::CommitDecision::kCommit));
    checkMetrics(expectedMetrics);
}

TEST_F(TransactionCoordinatorMetricsTest, SimpleTwoPhaseCommitRealCoordinator) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    LOGV2(22455, "Create the coordinator.");

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22456,
          "Start two phase commit (allow the coordinator to progress to writing the participant "
          "list).");

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration = Microseconds(0);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern",
                       BSON("mode"
                            << "alwaysOn"
                            << "data" << BSON("useUninterruptibleSleep" << 1)));
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22457, "Allow the coordinator to progress to waiting for votes.");

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration = Microseconds(0);
    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern",
                       BSON("mode"
                            << "off"));
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22458, "Allow the coordinator to progress to writing the decision.");

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    expectedStats.writingDecisionDuration = Microseconds(0);
    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern",
                       BSON("mode"
                            << "alwaysOn"
                            << "data" << BSON("useUninterruptibleSleep" << 1)));
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    waitUntilCoordinatorDocHasDecision();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22459, "Allow the coordinator to progress to waiting for acks.");

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern",
                       BSON("mode"
                            << "off"));
    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22460, "Allow the coordinator to progress to deleting the coordinator doc.");

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc",
                       BSON("mode"
                            << "alwaysOn"
                            << "data" << BSON("useUninterruptibleSleep" << 1)));
    // Respond to the second commit request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertCommitSentAndRespondWithSuccess();
    future = launchAsync([this] { assertCommitSentAndRespondWithSuccess(); });
    waitUntilNoCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22461, "Allow the coordinator to complete.");

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    expectedMetrics.currentDeletingCoordinatorDoc--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc",
                       BSON("mode"
                            << "off"));
    // The last thing the coordinator will do on the hijacked commit response thread is signal the
    // coordinator's completion.

    future.timed_get(kLongFutureTimeout);
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is logged since the coordination completed successfully.
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorIsCanceledWhileInactive) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Cancel the coordinator.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    coordinator.cancelIfCommitNotYetStarted();
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::TransactionCoordinatorCanceled);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsInactive) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingParticipantList) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the participant list.

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;

    FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern");
    coordinator.runCommit(operationContext(), kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    expectedMetrics.currentWritingParticipantList--;

    killClientOpCtx(getServiceContext(), "hangBeforeWaitingForParticipantListWriteConcern");
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedAtShutdown);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForVotes) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for votes.

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWaitingForVotes++;

    coordinator.runCommit(operationContext(), kTwoShardIdList);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    expectedMetrics.currentWaitingForVotes--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();

    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingDecision) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the decision.

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingDecisionDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingDecision++;

    FailPointEnableBlock fp("hangBeforeWaitingForDecisionWriteConcern");

    coordinator.runCommit(operationContext(), kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    waitUntilCoordinatorDocHasDecision();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    expectedMetrics.currentWritingDecision--;

    killClientOpCtx(getServiceContext(), "hangBeforeWaitingForDecisionWriteConcern");
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedAtShutdown);


    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForDecisionAcks) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for decision acks.

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWaitingForDecisionAcks++;

    coordinator.runCommit(operationContext(), kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsDeletingDoc) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is deleting the coordinator doc.

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentDeletingCoordinatorDoc++;

    FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc");

    coordinator.runCommit(operationContext(), kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    // The last thing the coordinator will do on the hijacked prepare response thread is
    // schedule the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();
    // Respond to the second commit request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertCommitSentAndRespondWithSuccess();
    future = launchAsync([this] { assertCommitSentAndRespondWithSuccess(); });
    waitUntilNoCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    expectedMetrics.currentDeletingCoordinatorDoc--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    // The last thing the coordinator will do on the hijacked commit response thread is signal
    // the coordinator's completion.
    future.timed_get(kLongFutureTimeout);
    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), DBException, ErrorCodes::InterruptedDueToReplStateChange);

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    stopCapturingLogMessages();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionAtLogLevelOne) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Debug(1)};
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionAtLogLevelZero) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionsUnderSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(operationContext(), kTwoShardIdList);

    tickSource()->advance(Milliseconds(99));

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    coordinator.onCompletion().get();
    stopCapturingLogMessages();

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(
    TransactionCoordinatorMetricsTest,
    DoesNotLogTransactionsUnderSlowMSThresholdEvenIfCoordinatorHasExistedForLongerThanSlowThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    tickSource()->advance(Milliseconds(101));

    coordinator.runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    coordinator.onCompletion().get();
    stopCapturingLogMessages();

    ASSERT_EQUALS(0, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionsOverSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    tickSource()->advance(Milliseconds(101));

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    coordinator.onCompletion().get();
    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countTextFormatLogLinesContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTransactionParameters) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    BSONObjBuilder lsidBob;
    _lsid.serialize(&lsidBob);
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON(
                      "attr" << BSON("parameters" << BSON(
                                         "lsid" << lsidBob.obj() << "txnNumber"
                                                << _txnNumberAndRetryCounter.getTxnNumber())))));
}

TEST_F(TransactionCoordinatorMetricsTest,
       SlowLogLineIncludesTerminationCauseAndCommitTimestampForCommitDecision) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(
                      BSON("attr" << BSON("terminationCause"
                                          << "committed"
                                          << "commitTimestamp" << Timestamp(1, 1).toBSON()))));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTerminationCauseForAbortDecision) {
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        coordinator.onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    stopCapturingLogMessages();

    ASSERT_EQUALS(1,
                  countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("terminationCause"
                                                                      << "aborted"))));

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON(
                "terminationDetails"
                << BSON("code" << 251 << "codeName"
                               << "NoSuchTransaction"
                               << "errmsg"
                               << "from shard s1 :: caused by :: No such transaction exists")))) +
            countBSONFormatLogLinesIsSubset(BSON(
                "attr" << BSON(
                    "terminationDetails"
                    << BSON("code" << 251 << "codeName"
                                   << "NoSuchTransaction"
                                   << "errmsg"
                                   << "from shard s2 :: caused by :: No such transaction exists"))))

    );
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesNumParticipants) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();

    ASSERT_EQUALS(1, countBSONFormatLogLinesIsSubset(BSON("attr" << BSON("numParticipants" << 2))));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesStepDurationsAndTotalDuration) {
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    {
        FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern",
                                BSON("useUninterruptibleSleep" << 1));

        coordinator.runCommit(operationContext(), kTwoShardIdList);
        waitUntilCoordinatorDocIsPresent();

        // Increase the duration spent writing the participant list.
        tickSource()->advance(Milliseconds(100));
    }

    waitUntilMessageSent();

    // Increase the duration spent waiting for votes.
    tickSource()->advance(Milliseconds(100));

    boost::optional<executor::NetworkTestEnv::FutureHandle<void>> futureOption;

    {
        FailPointEnableBlock fp("hangBeforeWaitingForDecisionWriteConcern",
                                BSON("useUninterruptibleSleep" << 1));

        // Respond to the second prepare request in a separate thread, because the coordinator will
        // hijack that thread to run its continuation.
        assertPrepareSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); }));
        waitUntilCoordinatorDocHasDecision();

        // Increase the duration spent writing the decision.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    futureOption->timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    // Increase the duration spent waiting for decision acks.
    tickSource()->advance(Milliseconds(100));

    {
        FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc",
                                BSON("useUninterruptibleSleep" << 1));

        // Respond to the second commit request in a separate thread, because the coordinator will
        // hijack that thread to run its continuation.
        assertCommitSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertCommitSentAndRespondWithSuccess(); }));
        waitUntilNoCoordinatorDocIsPresent();

        // Increase the duration spent deleting the coordinator doc.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked commit response thread is signal the
    // coordinator's completion.
    futureOption->timed_get(kLongFutureTimeout);
    coordinator.onCompletion().get();

    stopCapturingLogMessages();

    // Note: The waiting for decision acks and deleting coordinator doc durations are not reported.

    ASSERT_EQUALS(
        1,
        countBSONFormatLogLinesIsSubset(BSON(
            "attr" << BSON("stepDurations" << BSON(

                                                  "writingParticipantListMicros"
                                                  << 100000 << "waitingForVotesMicros" << 100000
                                                  << "writingDecisionMicros" << 100000
                                                  << "waitingForDecisionAcksMicros" << 100000
                                                  << "deletingCoordinatorDocMicros" << 100000)
                                           << "durationMillis" << 500))));
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesTotalCreated) {
    metrics()->incrementTotalCreated();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesTotalStartedTwoPhaseCommit) {
    metrics()->incrementTotalStartedTwoPhaseCommit();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentWritingParticipantList) {
    metrics()->incrementCurrentWritingParticipantList();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesCurrentWaitingForVotes) {
    metrics()->incrementCurrentWaitingForVotes();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesCurrentWritingDecision) {
    metrics()->incrementCurrentWritingDecision();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentWaitingForDecisionAcks) {
    metrics()->incrementCurrentWaitingForDecisionAcks();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentDeletingCoordinatorDoc) {
    metrics()->incrementCurrentDeletingCoordinatorDoc();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, RecoveryFromFailureIndicatedInReportState) {
    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    const auto assertRecoveryFlag = [&coordinator](bool expectedFlagValue) {
        BSONObjBuilder builder;
        coordinator.reportState(builder);
        auto reportDoc = builder.obj();
        auto coordinatorDoc = reportDoc.getObjectField("twoPhaseCommitCoordinator");
        ASSERT_EQ(coordinatorDoc.getBoolField("hasRecoveredFromFailover"), expectedFlagValue);
    };

    assertRecoveryFlag(false);

    TransactionCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setParticipants(kTwoShardIdList);
    coordinator.continueCommit(coordinatorDoc);

    assertRecoveryFlag(true);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorMetricsTest, ClientInformationIncludedInReportState) {
    const auto expectedAppName = std::string("Foo");
    associateClientMetadata(getClient(), expectedAppName);

    TransactionCoordinator coordinator(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    {
        BSONObjBuilder builder;
        coordinator.reportState(builder);
        BSONObj reportDoc = builder.obj();
        ASSERT_EQ(StringData(reportDoc.getStringField("desc")), "transaction coordinator");
        assertClientReportStateFields(reportDoc, expectedAppName, getClient()->getConnectionId());
    }

    const auto expectedAppName2 = std::string("Bar");
    associateClientMetadata(getClient(), expectedAppName2);

    coordinator.runCommit(operationContext(), kTwoShardIdList);

    {
        BSONObjBuilder builder;
        coordinator.reportState(builder);
        BSONObj reportDoc = builder.obj();
        ASSERT_EQ(StringData(reportDoc.getStringField("desc")), "transaction coordinator");
        assertClientReportStateFields(reportDoc, expectedAppName2, getClient()->getConnectionId());
    }

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    coordinator.onCompletion().get();
}
}  // namespace
}  // namespace mongo
