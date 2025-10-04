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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/s/transaction_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/single_transaction_coordinator_stats.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/s/transaction_coordinator_metrics_observer.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/string_map.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ratio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
const StatusWith<BSONObj> kAPIMismatchError =
    BSON("ok" << 0 << "code" << ErrorCodes::APIMismatchError << "errmsg"
              << "API parameter mismatch...");
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const Timestamp kDummyPrepareTimestamp = Timestamp(1, 1);
const std::vector<NamespaceString> kDummyAffectedNamespaces = {
    NamespaceString::createNamespaceString_forTest("test.test")};

StatusWith<BSONObj> makePrepareOkResponse(const Timestamp& timestamp,
                                          const std::vector<NamespaceString>& affectedNamespaces) {
    BSONArrayBuilder namespaces;
    for (const auto& nss : affectedNamespaces) {
        namespaces << nss.ns_forTest();
    }
    return BSON("ok" << 1 << "prepareTimestamp" << timestamp << "affectedNamespaces"
                     << namespaces.arr());
}

const StatusWith<BSONObj> kPrepareOk =
    makePrepareOkResponse(kDummyPrepareTimestamp, kDummyAffectedNamespaces);
const StatusWith<BSONObj> kPrepareOkNoTimestamp = BSON("ok" << 1);
const StatusWith<BSONObj> kTxnRetryCounterTooOld =
    BSON("ok" << 0 << "code" << ErrorCodes::TxnRetryCounterTooOld << "errmsg"
              << "txnRetryCounter is too old"
              << "txnRetryCounter" << 1);

template <typename NamespaceStringContainer>
static StringSet toStringSet(const NamespaceStringContainer& namespaces) {
    StringSet set;
    set.reserve(namespaces.size());
    for (const auto& nss : namespaces) {
        set.emplace(nss.ns_forTest());
    }
    return set;
}

/**
 * Searches for a client matching the name and mark the operation context as killed.
 */
void killClientOpCtx(ServiceContext* service,
                     const std::string& clientName,
                     ErrorCodes::Error error) {
    for (int retries = 0; retries < 20; retries++) {
        for (ServiceContext::LockedClientsCursor cursor(service); auto client = cursor.next();) {
            invariant(client);

            ClientLock lk(client);
            if (client->desc() == clientName) {
                if (auto opCtx = client->getOperationContext()) {
                    opCtx->getServiceContext()->killOperation(lk, opCtx, error);
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
            PrepareTransaction::kCommandName, kPrepareOk, defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithSuccess(const Timestamp& timestamp) {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        makePrepareOkResponse(timestamp, kDummyAffectedNamespaces),
                                        defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithAPIMismatchError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kAPIMismatchError,
                                        defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransaction,
                                        defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kRetryableError,
                                        defaultMajorityWriteConcernDoNotUse());
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
                dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace, BSONObj{}),
                IDLParserContext("dummy"));
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
    prepareCmd.setDbName(DatabaseName::kAdmin);
    prepareCmd.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    prepareCmd.setTxnNumber(txnNumberAndRetryCounter.getTxnNumber());
    prepareCmd.setTxnRetryCounter(txnNumberAndRetryCounter.getTxnRetryCounter());
    prepareCmd.setAutocommit(false);
    prepareCmd.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    return prepareCmd.toBSON();
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
       SendDecisionToParticipantShardInterpretsTwoPhaseDecisionAckErrorAsSuccess) {
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
       SendDecisionToParticipantShardInterpretsVoteToAbortErrorsAsFailure) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future =
        txn::sendDecisionToShard(getServiceContext(),
                                 aws,
                                 _lsid,
                                 _txnNumberAndRetryCounter,
                                 kTwoShardIdList[0],
                                 makeDummyPrepareCommand(_lsid, _txnNumberAndRetryCounter));

    // Ensure that the APIMismatchError (VoteAbortError category) is not interpreted as a success.
    // This allows it to be retried indefinitely, like any other error, even though such errors are
    // unexpected at this stage. Consequently, shutting down the coordinator will consistently
    // determine that the scheduler has not succeeded, leading to the retrial process failing with a
    // TransactionCoordinatorSteppingDown error.
    assertPrepareSentAndRespondWithAPIMismatchError();
    sleepmillis(10);
    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    advanceClockAndExecuteScheduledTasks();
    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
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
                [&](const executor::RemoteCommandRequest& request) {
                    return kPrepareOk;
                }});

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
                [&](const executor::RemoteCommandRequest& request) {
                    return kNoSuchTransaction;
                }});

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
    assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                    kPrepareOkNoTimestamp,
                                    defaultMajorityWriteConcernDoNotUse());

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
        defaultMajorityWriteConcernDoNotUse());

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

TEST_F(TransactionCoordinatorDriverTest, SendPrepareToShardsCollectsAffectedNamespaces) {
    const auto timestamp = Timestamp(1, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(),
                                   aws,
                                   _lsid,
                                   _txnNumberAndRetryCounter,
                                   APIParameters(),
                                   kTwoShardIdList);

    assertCommandSentAndRespondWith(
        PrepareTransaction::kCommandName,
        makePrepareOkResponse(timestamp,
                              {NamespaceString::createNamespaceString_forTest("db1.coll1"),
                               NamespaceString::createNamespaceString_forTest("db2.coll2")}),
        defaultMajorityWriteConcernDoNotUse());
    assertCommandSentAndRespondWith(
        PrepareTransaction::kCommandName,
        makePrepareOkResponse(timestamp,
                              {NamespaceString::createNamespaceString_forTest("db1.coll2"),
                               NamespaceString::createNamespaceString_forTest("db2.coll1")}),
        defaultMajorityWriteConcernDoNotUse());

    auto response = future.get();
    ASSERT_EQUALS(txn::CommitDecision::kCommit, response.decision().getDecision());
    StringSet expectedAffectedNamespaces{"db1.coll1", "db1.coll2", "db2.coll1", "db2.coll2"};
    ASSERT_EQUALS(expectedAffectedNamespaces, toStringSet(response.releaseAffectedNamespaces()));
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
        boost::optional<Timestamp> expectedCommitTimestamp = boost::none,
        boost::optional<std::vector<NamespaceString>> expectedAffectedNamespaces = boost::none) {
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

        ASSERT_EQUALS(expectedAffectedNamespaces.has_value(),
                      doc.getAffectedNamespaces().has_value());
        if (expectedAffectedNamespaces) {
            ASSERT_EQUALS(toStringSet(*expectedAffectedNamespaces),
                          toStringSet(*doc.getAffectedNamespaces()));
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
       PersistCommitDecisionWhenNoDocumentForTransactionExistsCanBeInterruptedAndReturnsError) {
    Future<repl::OpTime> future;

    {
        FailPointEnableBlock failpoint("hangBeforeWritingDecision");
        future = txn::persistDecision(
            *_aws,
            _lsid,
            _txnNumberAndRetryCounter,
            _participants,
            [&] {
                txn::CoordinatorCommitDecision decision(txn::CommitDecision::kCommit);
                decision.setCommitTimestamp(_commitTimestamp);
                return decision;
            }(),
            kDummyAffectedNamespaces);
        failpoint->waitForTimesEntered(failpoint.initialTimesEntered() + 1);
        _aws->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    }

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
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
    txn::persistDecision(
        *_aws,
        _lsid,
        txnNumberAndRetryCounter1,
        _participants,
        [&] {
            txn::CoordinatorCommitDecision decision(txn::CommitDecision::kAbort);
            decision.setAbortStatus(Status(ErrorCodes::NoSuchTransaction, "Test abort error"));
            return decision;
        }(),
        kDummyAffectedNamespaces)
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
    txn::persistDecision(
        *_aws,
        _lsid,
        txnNumberAndRetryCounter1,
        _participants,
        [&] {
            txn::CoordinatorCommitDecision decision(txn::CommitDecision::kAbort);
            decision.setAbortStatus(Status(ErrorCodes::NoSuchTransaction, "Test abort error"));
            return decision;
        }(),
        kDummyAffectedNamespaces)
        .get();
    txn::deleteCoordinatorDoc(*_aws, _lsid, txnNumberAndRetryCounter1).get();

    allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, txnNumberAndRetryCounter2, _participants);
}

class TransactionCoordinatorDecisionPersistenceTest
    : public TransactionCoordinatorDriverPersistenceTest {
protected:
    void persistDecisionExpectSuccess(
        OperationContext* opCtx,
        LogicalSessionId lsid,
        TxnNumberAndRetryCounter txnNumberAndRetryCounter,
        const std::vector<ShardId>& participants,
        const boost::optional<Timestamp>& commitTimestamp,
        const boost::optional<std::vector<NamespaceString>>& affectedNamespaces) {
        txn::persistDecision(
            *_aws,
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
                    decision.setAbortStatus(
                        Status(ErrorCodes::NoSuchTransaction, "Test abort status"));
                }
                return decision;
            }(),
            kDummyAffectedNamespaces)
            .get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        if (commitTimestamp) {
            bool useAffectedNamespaces = feature_flags::gFeatureFlagEndOfTransactionChangeEvent
                                             .isEnabledAndIgnoreFCVUnsafe();
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumberAndRetryCounter,
                                  participants,
                                  txn::CommitDecision::kCommit,
                                  *commitTimestamp,
                                  useAffectedNamespaces ? affectedNamespaces : boost::none);
        } else {
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumberAndRetryCounter,
                                  participants,
                                  txn::CommitDecision::kAbort);
        }
    }

    void persistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     boost::none,
                                     boost::none /* abort */);
    }

    void persistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     boost::none,
                                     boost::none /* abort */);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     boost::none,
                                     boost::none /* abort */);
    }

    void persistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     _commitTimestamp,
                                     kDummyAffectedNamespaces /* commit */);
    }

    void persistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     _commitTimestamp,
                                     kDummyAffectedNamespaces /* commit */);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     _commitTimestamp,
                                     kDummyAffectedNamespaces /* commit */);
    }

    void deleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     boost::none,
                                     boost::none /* abort */);
        deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumberAndRetryCounter);
    }

    void deleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds() {
        persistParticipantListExpectSuccess(
            operationContext(), _lsid, _txnNumberAndRetryCounter, _participants);
        persistDecisionExpectSuccess(operationContext(),
                                     _lsid,
                                     _txnNumberAndRetryCounter,
                                     _participants,
                                     _commitTimestamp,
                                     kDummyAffectedNamespaces /* commit */);
        deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumberAndRetryCounter);
    }
};

#define TEST_TRANSACTION_COORDINATOR_DECISION_PERSISTENCE(Fixture)                      \
    TEST_F(Fixture, PersistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds) {    \
        persistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds();                \
    }                                                                                   \
    TEST_F(Fixture, PersistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds) {   \
        persistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds();               \
    }                                                                                   \
    TEST_F(Fixture, PersistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds) {   \
        persistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds();               \
    }                                                                                   \
    TEST_F(Fixture, PersistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds) {  \
        persistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds();              \
    }                                                                                   \
    TEST_F(Fixture, DeleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds) {  \
        deleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds();              \
    }                                                                                   \
    TEST_F(Fixture, DeleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds) { \
        deleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds();             \
    }

class TransactionCoordinatorDecisionPersistenceTestWithEOTChangeEventTrue
    : public TransactionCoordinatorDecisionPersistenceTest {
public:
    void setUp() override {
        _controller.emplace("featureFlagEndOfTransactionChangeEvent", true);
        TransactionCoordinatorDecisionPersistenceTest::setUp();
    }
    void tearDown() override {
        TransactionCoordinatorDecisionPersistenceTest::tearDown();
        _controller.reset();
    }

private:
    boost::optional<RAIIServerParameterControllerForTest> _controller;
};

class TransactionCoordinatorDecisionPersistenceTestWithEOTChangeEventFalse
    : public TransactionCoordinatorDecisionPersistenceTest {
public:
    void setUp() override {
        _controller.emplace("featureFlagEndOfTransactionChangeEvent", false);
        TransactionCoordinatorDecisionPersistenceTest::setUp();
    }
    void tearDown() override {
        TransactionCoordinatorDecisionPersistenceTest::tearDown();
        _controller.reset();
    }

private:
    boost::optional<RAIIServerParameterControllerForTest> _controller;
};

TEST_TRANSACTION_COORDINATOR_DECISION_PERSISTENCE(
    TransactionCoordinatorDecisionPersistenceTestWithEOTChangeEventTrue);
TEST_TRANSACTION_COORDINATOR_DECISION_PERSISTENCE(
    TransactionCoordinatorDecisionPersistenceTestWithEOTChangeEventFalse);

using TransactionCoordinatorTest = TransactionCoordinatorTestBase;

TEST_F(TransactionCoordinatorTest, RunCommitProducesCommitDecisionOnTwoCommitResponses) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnAbortAndCommitResponses) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) {
                    return kPrepareOk;
                }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnCommitAndAbortResponsesNoSuchTransaction) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) {
                    return kNoSuchTransaction;
                }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnCommitAndAbortResponsesTxnRetryCounterTooOld) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) {
                    return kTxnRetryCounterTooOld;
                }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::TxnRetryCounterTooOld);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::TxnRetryCounterTooOld);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnSingleAbortResponseOnly) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    assertPrepareSentAndRespondWithNoSuchTransaction();
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneCommitResponseAndOneAbortResponseAfterRetry) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    // One participant votes commit and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) {
                    return kRetryableError;
                }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the scheduled retry executes

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithNoSuchTransaction();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneAbortResponseAndOneRetryableAbortResponse) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

    // One participant votes abort and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) {
                    return kRetryableError;
                }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesCommitDecisionOnCommitAfterMultipleNetworkRetries) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

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

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    coordinator->onCompletion().get();
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesReadConcernMajorityNotEnabledIfEitherShardReturnsIt) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    auto commitDecisionFuture = coordinator->getDecision();

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
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(coordinator->onCompletion().get(),
                       AssertionException,
                       ErrorCodes::ReadConcernMajorityNotEnabled);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesEndOfTransactionOplogEntry) {
    RAIIServerParameterControllerForTest controller("featureFlagEndOfTransactionChangeEvent", true);
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    coordinator->runCommit(operationContext(), kOneShardIdList);
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    coordinator->onCompletion().get();
    coordinator->shutdown();

    BSONArrayBuilder namespaces;
    for (const auto& nss : kDummyAffectedNamespaces) {
        namespaces.append(nss.ns_forTest());
    }
    BSONObj expectedO2 =
        BSON("endOfTransaction" << namespaces.arr() << repl::OplogEntry::kSessionIdFieldName
                                << _lsid.toBSON() << repl::OplogEntry::kTxnNumberFieldName
                                << _txnNumberAndRetryCounter.getTxnNumber());

    DBDirectClient dbClient(operationContext());
    auto oplogEntry = dbClient.findOne(NamespaceString::kRsOplogNamespace,
                                       BSON("op" << "n"
                                                 << "o.msg.endOfTransaction" << 1));
    auto o2 = oplogEntry.getField("o2");
    ASSERT_EQ(o2.type(), BSONType::object);
    ASSERT_BSONOBJ_EQ(o2.Obj(), expectedO2);
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

    static constexpr size_t kStepCount =
        static_cast<size_t>(TransactionCoordinator::Step::kLastStep) + 1;

    struct Stats {
        boost::optional<Date_t> createTime;
        boost::optional<Date_t> endTime;
        std::vector<boost::optional<Date_t>> stepStartTimes{kStepCount, boost::none};

        boost::optional<Microseconds> totalDuration;
        boost::optional<Microseconds> twoPhaseCommitDuration;
        std::vector<boost::optional<Microseconds>> stepDurations{kStepCount, boost::none};
    };

    void checkStats(const SingleTransactionCoordinatorStats& stats, const Stats& expected) {
        if (expected.createTime) {
            ASSERT_EQ(*expected.createTime, stats.getCreateTime());
        }
        if (expected.endTime) {
            ASSERT_EQ(*expected.endTime, stats.getEndTime());
        }
        if (expected.totalDuration) {
            ASSERT_EQ(*expected.totalDuration,
                      stats.getDurationSinceCreation(tickSource(), tickSource()->getTicks()));
        }
        if (expected.twoPhaseCommitDuration) {
            ASSERT_EQ(*expected.twoPhaseCommitDuration,
                      stats.getTwoPhaseCommitDuration(tickSource(), tickSource()->getTicks()));
        }

        size_t startIndex =
            static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
        size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
        for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
            auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
            if (expected.stepStartTimes[stepIndex]) {
                ASSERT_EQ(*expected.stepStartTimes[stepIndex], stats.getStepStartTime(step))
                    << "Step: " << TransactionCoordinator::toString(step);
            }
            if (expected.stepDurations[stepIndex]) {
                ASSERT_EQ(*expected.stepDurations[stepIndex],
                          stats.getStepDuration(step, tickSource(), tickSource()->getTicks()))
                    << "Step: " << TransactionCoordinator::toString(step);
            }
        }
    }

    struct Metrics {
        // Totals
        std::int64_t totalCreated{0};
        std::int64_t totalStartedTwoPhaseCommit{0};
        std::int64_t totalAbortedTwoPhaseCommit{0};
        std::int64_t totalCommittedTwoPhaseCommit{0};

        // Current in steps
        std::vector<std::int64_t> currentInSteps = std::vector<std::int64_t>(kStepCount, 0);
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
        size_t startIndex =
            static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
        size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
        for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
            auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
            ASSERT_EQ(expectedMetrics.currentInSteps[stepIndex], metrics()->getCurrentInStep(step));
        }
    }

    void checkServerStatus() {
        TransactionCoordinatorsSSS tcsss("testSection", ClusterRole::None);
        BSONElement dummy;
        const auto serverStatusSection = tcsss.generateSection(operationContext(), dummy);
        ASSERT_EQ(metrics()->getTotalCreated(), serverStatusSection["totalCreated"].Long());
        ASSERT_EQ(metrics()->getTotalStartedTwoPhaseCommit(),
                  serverStatusSection["totalStartedTwoPhaseCommit"].Long());

        size_t startIndex =
            static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
        size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
        for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
            auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
            std::string stepName = TransactionCoordinator::toString(step);
            ASSERT_EQ(metrics()->getCurrentInStep(step),
                      serverStatusSection.getObjectField("currentInSteps")[stepName].Long())
                << "Step: " << stepName;
        }
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
        auto coordinator = std::make_shared<TransactionCoordinator>(
            operationContext(),
            _lsid,
            _txnNumberAndRetryCounter,
            std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
            Date_t::max());
        coordinator->start(operationContext());

        coordinator->runCommit(operationContext(), kTwoShardIdList);

        assertPrepareSentAndRespondWithSuccess();
        assertPrepareSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();

        executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

        coordinator->onCompletion().get();
        coordinator->shutdown();
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

    expectedStats.twoPhaseCommitDuration = Microseconds(0);

    size_t startIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
    TransactionCoordinator::Step previousStep = TransactionCoordinator::Step::kInactive;
    for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
        auto step = static_cast<TransactionCoordinator::Step>(stepIndex);

        // Stats are updated on step start
        expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
        expectedStats.stepDurations[stepIndex] = Microseconds(0);
        coordinatorObserver.onStartStep(
            step, previousStep, metrics(), tickSource(), clockSource()->now());
        checkStats(stats, expectedStats);

        // Advancing the time causes the total duration, two-phase commit duration, and duration
        // of the current step to increase.
        tickSource()->advance(Microseconds(100));
        expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
        expectedStats.twoPhaseCommitDuration =
            *expectedStats.twoPhaseCommitDuration + Microseconds(100);
        expectedStats.stepDurations[stepIndex] =
            *expectedStats.stepDurations[stepIndex] + Microseconds(100);
        checkStats(stats, expectedStats);

        previousStep = step;
    }

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

    // Metrics are updated on start of each step.
    size_t startIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
    TransactionCoordinator::Step previousStep = TransactionCoordinator::Step::kInactive;
    expectedMetrics.totalStartedTwoPhaseCommit++;
    for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
        auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
        expectedMetrics.currentInSteps[stepIndex]++;
        if (stepIndex - 1 >= startIndex) {
            expectedMetrics.currentInSteps[stepIndex - 1]--;
        }
        coordinatorObserver.onStartStep(
            step, previousStep, metrics(), tickSource(), clockSource()->now());
        checkMetrics(expectedMetrics);

        previousStep = step;
    }

    // Metrics are updated on onEnd.
    expectedMetrics.currentInSteps[lastIndex]--;
    expectedMetrics.totalAbortedTwoPhaseCommit++;
    coordinatorObserver.onEnd(metrics(),
                              tickSource(),
                              clockSource()->now(),
                              TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                              CoordinatorCommitDecision(txn::CommitDecision::kAbort));
    checkMetrics(expectedMetrics);
}

TEST_F(TransactionCoordinatorMetricsTest, ServerWideMetricsSimpleTwoPhaseCommitTwoCoordinators) {
    std::vector<TransactionCoordinatorMetricsObserver> coordinatorObservers;
    coordinatorObservers.resize(2);
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Increment each coordinator one step at a time.
    for (auto& observer : coordinatorObservers) {
        expectedMetrics.totalCreated++;
        observer.onCreate(metrics(), tickSource(), clockSource()->now());
        checkMetrics(expectedMetrics);
    }

    size_t startIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
    TransactionCoordinator::Step previousStep = TransactionCoordinator::Step::kInactive;
    for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
        auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
        for (auto& observer : coordinatorObservers) {
            expectedMetrics.currentInSteps[stepIndex]++;
            if (stepIndex - 1 >= startIndex) {
                expectedMetrics.currentInSteps[stepIndex - 1]--;
            } else {
                expectedMetrics.totalStartedTwoPhaseCommit++;
            }
            observer.onStartStep(step, previousStep, metrics(), tickSource(), clockSource()->now());
            checkMetrics(expectedMetrics);
        }

        previousStep = step;
    }

    for (auto& observer : coordinatorObservers) {
        expectedMetrics.currentInSteps[lastIndex]--;
        expectedMetrics.totalAbortedTwoPhaseCommit++;
        observer.onEnd(metrics(),
                       tickSource(),
                       clockSource()->now(),
                       TransactionCoordinator::Step::kDeletingCoordinatorDoc,
                       CoordinatorCommitDecision(txn::CommitDecision::kAbort));
        checkMetrics(expectedMetrics);
    }
}

TEST_F(TransactionCoordinatorMetricsTest, SimpleTwoPhaseCommitRealCoordinator) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    LOGV2(22455, "Create the coordinator.");

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22456,
          "Start two phase commit (allow the coordinator to progress to writing the participant "
          "list).");

    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration = Microseconds(0);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern",
                       BSON("mode" << "alwaysOn"
                                   << "data" << BSON("useUninterruptibleSleep" << 1)));
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22457, "Allow the coordinator to progress to waiting for votes.");

    stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWaitingForVotes);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex - 1] =
        *expectedStats.stepDurations[stepIndex - 1] + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.currentInSteps[stepIndex - 1]--;
    expectedMetrics.currentInSteps[stepIndex]++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern", BSON("mode" << "off"));
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22458, "Allow the coordinator to progress to writing the decision.");

    stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingDecision);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex - 1] =
        *expectedStats.stepDurations[stepIndex - 1] + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.currentInSteps[stepIndex - 1]--;
    expectedMetrics.currentInSteps[stepIndex]++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern",
                       BSON("mode" << "alwaysOn"
                                   << "data" << BSON("useUninterruptibleSleep" << 1)));
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    waitUntilCoordinatorDocHasDecision();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22459, "Allow the coordinator to progress to waiting for acks.");

    stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWaitingForDecisionAcks);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex - 1] =
        *expectedStats.stepDurations[stepIndex - 1] + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.currentInSteps[stepIndex - 1]--;
    expectedMetrics.currentInSteps[stepIndex]++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern", BSON("mode" << "off"));
    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    LOGV2(22460, "Allow the coordinator to progress to deleting the coordinator doc.");

    size_t previousStepIndex = stepIndex;
    stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.stepDurations[previousStepIndex] =
        *expectedStats.stepDurations[previousStepIndex] + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.currentInSteps[previousStepIndex]--;
    expectedMetrics.currentInSteps[stepIndex]++;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc",
                       BSON("mode" << "alwaysOn"
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
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc", BSON("mode" << "off"));
    // The last thing the coordinator will do on the hijacked commit response thread is signal
    // the coordinator's completion.

    future.timed_get(kLongFutureTimeout);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    coordinator->onCompletion().get();
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is logged since the coordination completed successfully.
    ASSERT_EQUALS(1, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorIsCanceledWhileInactive) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Cancel the coordinator.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    coordinator->cancelIfCommitNotYetStarted();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), DBException, ErrorCodes::TransactionCoordinatorCanceled);
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsInactive) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(coordinator->onCompletion().get(),
                       DBException,
                       ErrorCodes::InterruptedDueToReplStateChange);
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingParticipantList) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the participant list.
    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern");
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;

    killClientOpCtx(getServiceContext(),
                    "hangBeforeWaitingForParticipantListWriteConcern",
                    ErrorCodes::InterruptedAtShutdown);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), DBException, ErrorCodes::InterruptedAtShutdown);
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForVotes) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for votes.
    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWaitingForVotes);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    coordinator->runCommit(operationContext(), kTwoShardIdList);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();

    ASSERT_THROWS_CODE(coordinator->onCompletion().get(),
                       DBException,
                       ErrorCodes::InterruptedDueToReplStateChange);
    coordinator->shutdown();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingDecision) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the decision.

    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingDecision);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    FailPointEnableBlock fp("hangBeforeWaitingForDecisionWriteConcern");

    coordinator->runCommit(operationContext(), kTwoShardIdList);
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
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;

    killClientOpCtx(getServiceContext(),
                    "hangBeforeWaitingForDecisionWriteConcern",
                    ErrorCodes::InterruptedAtShutdown);
    future.timed_get(kLongFutureTimeout);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), DBException, ErrorCodes::InterruptedAtShutdown);
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForDecisionAcks) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for decision acks.
    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWaitingForDecisionAcks);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    coordinator->runCommit(operationContext(), kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    // The last thing the coordinator will do on the hijacked prepare response thread is
    // schedule the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();
    ASSERT_THROWS_CODE(coordinator->onCompletion().get(),
                       DBException,
                       ErrorCodes::InterruptedDueToReplStateChange);
    coordinator->shutdown();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsDeletingDoc) {
    unittest::LogCaptureGuard logs;

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(), _lsid, _txnNumberAndRetryCounter, std::move(aws), Date_t::max());
    coordinator->start(operationContext());
    const auto& stats =
        coordinator->getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is deleting the coordinator doc.
    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    expectedStats.stepStartTimes[stepIndex] = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.stepDurations[stepIndex] = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentInSteps[stepIndex]++;

    FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc");

    coordinator->runCommit(operationContext(), kTwoShardIdList);
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
    expectedStats.stepDurations[stepIndex] =
        *expectedStats.stepDurations[stepIndex] + Microseconds(100);
    expectedMetrics.currentInSteps[stepIndex]--;
    expectedMetrics.totalCommittedTwoPhaseCommit++;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    // The last thing the coordinator will do on the hijacked commit response thread is signal
    // the coordinator's completion.
    future.timed_get(kLongFutureTimeout);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(coordinator->onCompletion().get(),
                       DBException,
                       ErrorCodes::InterruptedDueToReplStateChange);
    coordinator->shutdown();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    logs.stop();

    // Slow log line is not logged since the coordination did not complete successfully.
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       MetricsCorrectlyUpdatedWhenAbortDecisionErrorWhenWritingParticipantsList) {
    Stats expectedStats;
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Create the coordinator.
    expectedMetrics.totalCreated++;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::now() + Seconds(1));
    coordinator->start(operationContext());

    // Wait until the coordinator is writing the participant list.
    auto participantListFp =
        globalFailPointRegistry().find("hangBeforeWaitingForParticipantListWriteConcern");
    auto initTimesEntered = participantListFp->setMode(FailPoint::alwaysOn);
    coordinator->runCommit(operationContext(), kTwoShardIdList);
    participantListFp->waitForTimesEntered(initTimesEntered + 1);

    // We expect the "currentInSteps" metric for the kWritingParticipantList to be 1. All other step
    // metrics should be 0 (default).
    size_t stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    expectedMetrics.currentInSteps[stepIndex]++;
    expectedMetrics.totalStartedTwoPhaseCommit++;
    checkMetrics(expectedMetrics);
    checkServerStatus();

    // Force a "TransactionCoordinatorReachedAbortDecision" error while writing the participant
    // list. The coordinator should skip the "waitingForVotes" phase, and jump straight to the
    // "writingDecision" phase.
    FailPointEnableBlock decisionFp("hangBeforeWaitingForDecisionWriteConcern");
    killClientOpCtx(getServiceContext(),
                    "hangBeforeWaitingForParticipantListWriteConcern",
                    ErrorCodes::TransactionCoordinatorReachedAbortDecision);
    participantListFp->setMode(FailPoint::off);
    decisionFp->waitForTimesEntered(decisionFp.initialTimesEntered() + 1);

    // We now expect the "currentInSteps" metric for kWritingParticipantList to be 0, and for it to
    // be 1 for "kWritingDecision". All other steps, including "kWaitingForVotes" should be 0.
    expectedMetrics.currentInSteps[stepIndex]--;
    stepIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingDecision);
    expectedMetrics.currentInSteps[stepIndex]++;
    checkMetrics(expectedMetrics);
    checkServerStatus();

    // Force the coordinator to stop.
    killClientOpCtx(getServiceContext(),
                    "hangBeforeWaitingForDecisionWriteConcern",
                    ErrorCodes::InterruptedAtShutdown);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), DBException, ErrorCodes::InterruptedAtShutdown);
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionAtLogLevelOne) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Debug(1)};
    unittest::LogCaptureGuard logs;
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(1, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionAtLogLevelZero) {
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    unittest::LogCaptureGuard logs;
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionsUnderSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds
    // the slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS.store(100);
    unittest::LogCaptureGuard logs;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    coordinator->runCommit(operationContext(), kTwoShardIdList);

    tickSource()->advance(Milliseconds(99));

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
    logs.stop();

    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(
    TransactionCoordinatorMetricsTest,
    DoesNotLogTransactionsUnderSlowMSThresholdEvenIfCoordinatorHasExistedForLongerThanSlowThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds
    // the slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS.store(100);
    unittest::LogCaptureGuard logs;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    tickSource()->advance(Milliseconds(101));

    coordinator->runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
    logs.stop();

    ASSERT_EQUALS(0, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionsOverSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds
    // the slowMS setting.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kTransaction,
                                                              logv2::LogSeverity::Log()};
    serverGlobalParams.slowMS.store(100);
    unittest::LogCaptureGuard logs;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    coordinator->runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    tickSource()->advance(Milliseconds(101));

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
    logs.stop();

    ASSERT_EQUALS(1, logs.countTextContaining("two-phase commit"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTransactionParameters) {
    unittest::LogCaptureGuard logs;
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    logs.stop();
    BSONObjBuilder lsidBob;
    _lsid.serialize(&lsidBob);
    ASSERT_EQUALS(1,
                  logs.countBSONContainingSubset(BSON(
                      "attr" << BSON("parameters" << BSON(
                                         "lsid" << lsidBob.obj() << "txnNumber"
                                                << _txnNumberAndRetryCounter.getTxnNumber())))));
}

TEST_F(TransactionCoordinatorMetricsTest,
       SlowLogLineIncludesTerminationCauseAndCommitTimestampForCommitDecision) {
    unittest::LogCaptureGuard logs;
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    logs.stop();
    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON(
            "attr" << BSON("terminationCause" << "committed"
                                              << "commitTimestamp" << Timestamp(1, 1).toBSON()))));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTerminationCauseForAbortDecision) {
    unittest::LogCaptureGuard logs;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    coordinator->runCommit(operationContext(), kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    ASSERT_THROWS_CODE(
        coordinator->onCompletion().get(), AssertionException, ErrorCodes::NoSuchTransaction);
    coordinator->shutdown();
    logs.stop();

    ASSERT_EQUALS(
        1, logs.countBSONContainingSubset(BSON("attr" << BSON("terminationCause" << "aborted"))));

    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON(
            "attr" << BSON(
                "terminationDetails"
                << BSON("code" << 251 << "codeName"
                               << "NoSuchTransaction"
                               << "errmsg"
                               << "from shard s1 :: caused by :: No such transaction exists")))) +
            logs.countBSONContainingSubset(BSON(
                "attr" << BSON(
                    "terminationDetails"
                    << BSON("code" << 251 << "codeName"
                                   << "NoSuchTransaction"
                                   << "errmsg"
                                   << "from shard s2 :: caused by :: No such transaction exists"))))

    );
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesNumParticipants) {
    unittest::LogCaptureGuard logs;
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();

    ASSERT_EQUALS(1, logs.countBSONContainingSubset(BSON("attr" << BSON("numParticipants" << 2))));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesStepDurationsAndTotalDuration) {
    unittest::LogCaptureGuard logs;

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    {
        FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern",
                                BSON("useUninterruptibleSleep" << 1));

        coordinator->runCommit(operationContext(), kTwoShardIdList);
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

        // Respond to the second prepare request in a separate thread, because the coordinator
        // will hijack that thread to run its continuation.
        assertPrepareSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); }));
        waitUntilCoordinatorDocHasDecision();

        // Increase the duration spent writing the decision.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked prepare response thread is
    // schedule the commitTransaction network requests.
    futureOption->timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    // Increase the duration spent waiting for decision acks.
    tickSource()->advance(Milliseconds(100));

    {
        FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc",
                                BSON("useUninterruptibleSleep" << 1));

        // Respond to the second commit request in a separate thread, because the coordinator
        // will hijack that thread to run its continuation.
        assertCommitSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertCommitSentAndRespondWithSuccess(); }));
        waitUntilNoCoordinatorDocIsPresent();

        // Increase the duration spent deleting the coordinator doc.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked commit response thread is signal
    // the coordinator's completion.
    futureOption->timed_get(kLongFutureTimeout);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
    coordinator->onCompletion().get();
    coordinator->shutdown();

    logs.stop();

    // Note: The waiting for decision acks and deleting coordinator doc durations are not
    // reported.

    ASSERT_EQUALS(
        1,
        logs.countBSONContainingSubset(BSON(
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

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesAllSteps) {
    size_t startIndex = static_cast<size_t>(TransactionCoordinator::Step::kWritingParticipantList);
    size_t lastIndex = static_cast<size_t>(TransactionCoordinator::Step::kLastStep);
    for (size_t stepIndex = startIndex; stepIndex <= lastIndex; ++stepIndex) {
        auto step = static_cast<TransactionCoordinator::Step>(stepIndex);
        metrics()->incrementCurrentInStep(step);
        checkServerStatus();
    }
}

TEST_F(TransactionCoordinatorMetricsTest, RecoveryFromFailureIndicatedInReportState) {
    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    const auto assertRecoveryFlag = [opCtx = operationContext(),
                                     &coordinator](bool expectedFlagValue) {
        BSONObjBuilder builder;
        coordinator->reportState(opCtx, builder);
        auto reportDoc = builder.obj();
        auto coordinatorDoc = reportDoc.getObjectField("twoPhaseCommitCoordinator");
        ASSERT_EQ(coordinatorDoc.getBoolField("hasRecoveredFromFailover"), expectedFlagValue);
    };

    assertRecoveryFlag(false);

    TransactionCoordinatorDocument coordinatorDoc;
    coordinatorDoc.setParticipants(kTwoShardIdList);
    coordinator->continueCommit(coordinatorDoc);

    assertRecoveryFlag(true);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
}

TEST_F(TransactionCoordinatorMetricsTest, ClientInformationIncludedInReportState) {
    const auto expectedAppName = std::string("Foo");
    associateClientMetadata(getClient(), expectedAppName);

    auto coordinator = std::make_shared<TransactionCoordinator>(
        operationContext(),
        _lsid,
        _txnNumberAndRetryCounter,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    coordinator->start(operationContext());

    {
        BSONObjBuilder builder;
        coordinator->reportState(operationContext(), builder);
        BSONObj reportDoc = builder.obj();
        ASSERT_EQ(StringData(reportDoc.getStringField("desc")), "transaction coordinator");
        assertClientReportStateFields(reportDoc, expectedAppName, getClient()->getConnectionId());
    }

    const auto expectedAppName2 = std::string("Bar");
    associateClientMetadata(getClient(), expectedAppName2);

    coordinator->runCommit(operationContext(), kTwoShardIdList);

    {
        BSONObjBuilder builder;
        coordinator->reportState(operationContext(), builder);
        BSONObj reportDoc = builder.obj();
        ASSERT_EQ(StringData(reportDoc.getStringField("desc")), "transaction coordinator");
        assertClientReportStateFields(reportDoc, expectedAppName2, getClient()->getConnectionId());
    }

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    coordinator->onCompletion().get();
    coordinator->shutdown();
}

}  // namespace
}  // namespace mongo
