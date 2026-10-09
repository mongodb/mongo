// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/multi_statement_transaction_requests_sender.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#include <string_view>
#include <system_error>

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"

namespace mongo {

namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("test.bar");
const ShardId kTestRemoteShardId1 = ShardId("FakeShard1");
const HostAndPort kTestRemoteHostAndPort1 = HostAndPort("FakeShard1Host", 12345);
const ShardId kTestRemoteShardId2 = ShardId("FakeShard2");
const HostAndPort kTestRemoteHostAndPort2 = HostAndPort("FakeShard2Host", 12345);

class RouterMultiStatementTransactionRequestsSenderTest
    : public virtual service_context_test::RouterRoleOverride,
      public ShardingTestFixture {
public:
    RouterMultiStatementTransactionRequestsSenderTest() {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(HostAndPort("FakeConfigHost", 12345));

        // Populate the shardRegistry
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId1, kTestRemoteHostAndPort1));
        addRemoteShards(shardInfos);
    }

protected:
    void checkRequestMetadata(executor::RemoteCommandRequest request,
                              std::string_view expectedCmd,
                              bool expectStartTxnFields,
                              bool expectStartOrContinueTxnFields) {
        ASSERT(request.cmdObj.hasField(expectedCmd));
        if (expectStartTxnFields || expectStartOrContinueTxnFields) {
            if (expectStartTxnFields) {
                ASSERT(request.cmdObj.hasField("startTransaction"));
            } else {  // expectStartOrContinueTxnFields
                ASSERT(request.cmdObj.hasField("startOrContinueTransaction"));
            }
            ASSERT(request.cmdObj.hasField("autocommit"));
            ASSERT(request.cmdObj.hasField("txnNumber"));
            return;
        }

        ASSERT(!request.cmdObj.hasField("startTransaction"));
        ASSERT(!request.cmdObj.hasField("startOrContinueTransaction"));
        ASSERT(!request.cmdObj.hasField("autocommit"));
        ASSERT(!request.cmdObj.hasField("txnNumber"));
    }
};

TEST_F(RouterMultiStatementTransactionRequestsSenderTest, TxnDetailsNotAppendedIfNoTxnRouter) {
    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId1, BSON(cmdName << "bar")}};

    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                kTestNss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    auto future = launchAsync([&]() {
        auto response = msars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    onCommand([&](const auto& request) {
        checkRequestMetadata(request,
                             cmdName,
                             /*expectStartTxnFields=*/false,
                             /*expectStartOrContinueTxnFields=*/false);
        return BSON("ok" << true);
    });

    future.default_timed_get();
}


TEST_F(RouterMultiStatementTransactionRequestsSenderTest, TxnDetailsAreAppendedIfTxnRouter) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(
            operationContext(), TxnNumber(0), TransactionRouter::TransactionActions::kStart);
    TransactionRouter::get(operationContext()).setDefaultAtClusterTime(operationContext());

    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId1, BSON("find" << "bar")}};

    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                kTestNss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    auto future = launchAsync([&]() {
        auto response = msars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    onCommand([&](const auto& request) {
        checkRequestMetadata(request,
                             cmdName,
                             /*expectStartTxnFields=*/true,
                             /*expectStartOrContinueTxnFields=*/false);

        // The TransactionRouter will throw when parsing this response if
        // "processParticipantResponse" does not exist
        return BSON("ok" << true << "readOnly" << true);
    });

    future.default_timed_get();
}

TEST_F(RouterMultiStatementTransactionRequestsSenderTest, TxnDetailsAreAppendedIfSubTxnRouter) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();
    operationContext()->setActiveTransactionParticipant();
    repl::ReadConcernArgs readConcernArgs;
    ASSERT_OK(readConcernArgs.initialize(
        BSON("find" << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                    << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                            << LogicalTime(Timestamp(1, 0)).asTimestamp()
                            << repl::ReadConcernArgs::kLevelFieldName << "snapshot"))));
    repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    RouterOperationContextSession rocs(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(operationContext(),
                            TxnNumber(0),
                            TransactionRouter::TransactionActions::kStartOrContinue);

    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId1, BSON("find" << "bar")}};

    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                kTestNss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    auto future = launchAsync([&]() {
        auto response = msars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    onCommand([&](const auto& request) {
        checkRequestMetadata(request,
                             cmdName,
                             /*expectStartTxnFields=*/false,
                             /*expectStartOrContinueTxnFields=*/true);

        // The TransactionRouter will throw when parsing this response if
        // "processParticipantResponse" does not exist
        return BSON("ok" << true << "readOnly" << true);
    });

    future.default_timed_get();
}

// A metadata-validation failure on one response must not abandon sibling responses: next() drains
// the remaining responses (enrolling their announced participants) and only then rethrows the
// original failure.
TEST_F(RouterMultiStatementTransactionRequestsSenderTest, NextDrainsSiblingsAfterMetadataFailure) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(operationContext());
    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), TxnNumber(0), TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    const ShardId kTestRemoteShardId3 = ShardId("FakeShard3");
    const ShardId kTestRemoteShardId4 = ShardId("FakeShard4");

    // setUp() registered only shard1; the sender targets shard2 as well. addRemoteShards()
    // re-mocks the config.shards find to return exactly the shards passed, so both shards must be
    // registered in one call — a second call would evict shard1 and its request would never
    // resolve a host.
    std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
    shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId1, kTestRemoteHostAndPort1));
    shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId2, kTestRemoteHostAndPort2));
    addRemoteShards(shardInfos);

    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId1, BSON("find" << "bar")}, {kTestRemoteShardId2, BSON("find" << "bar")}};

    // Construction attaches txn fields, enrolling both targeted shards.
    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                kTestNss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    // Record shard3's term (5) as if an earlier response announced it.
    txnRouter.processParticipantResponse(
        operationContext(),
        kTestRemoteShardId1,
        TransactionRouter::Router::parseParticipantResponseMetadata(
            BSON("ok" << 1 << "readOnly" << true << "additionalParticipants"
                      << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId3 << "readOnly" << true
                                                   << "term" << 5LL)))));
    auto recordedParticipant = txnRouter.getParticipant(kTestRemoteShardId3);
    ASSERT(recordedParticipant);
    ASSERT_EQ(*recordedParticipant->term, 5);

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(msars.next(), AssertionException, ErrorCodes::NoSuchTransaction);
    });

    // The first response announces shard3 with a changed term (6): validation fails. The second
    // (sibling) response announces shard4 and must still be drained and enrolled before the
    // original error surfaces.
    {
        // Deliver the second response only after next() has consumed the first, so the
        // arrival order is deterministic.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            return BSON("ok" << 1 << "readOnly" << true << "additionalParticipants"
                             << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId3 << "readOnly"
                                                          << true << "term" << 6LL)));
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return BSON("ok" << 1 << "readOnly" << true << "additionalParticipants"
                         << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId4 << "readOnly" << true
                                                      << "term" << 7LL)));
    });

    future.default_timed_get();

    // The sibling's announced participant was enrolled during the drain, and shard3's recorded term
    // was not overwritten by the mismatching observation.
    ASSERT(txnRouter.getParticipant(kTestRemoteShardId4));
    recordedParticipant = txnRouter.getParticipant(kTestRemoteShardId3);
    ASSERT(recordedParticipant);
    ASSERT_EQ(*recordedParticipant->term, 5);
}

// A metadata-validation failure stops retries before draining: a sibling's retryable network
// error must not schedule a retry and stall the drain on a response that never comes.
TEST_F(RouterMultiStatementTransactionRequestsSenderTest,
       MetadataFailureStopsRetryingBeforeDraining) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(operationContext());
    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.beginOrContinueTxn(
        operationContext(), TxnNumber(0), TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(operationContext());

    const ShardId kTestRemoteShardId3 = ShardId("FakeShard3");

    // Register both shards in one call; a second addRemoteShards() would evict the first.
    std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
    shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId1, kTestRemoteHostAndPort1));
    shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId2, kTestRemoteHostAndPort2));
    addRemoteShards(shardInfos);

    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId1, BSON("find" << "bar")}, {kTestRemoteShardId2, BSON("find" << "bar")}};

    // Pre-enroll both shards so the sender's requests continue the transaction instead of
    // starting it: startTransaction requests have their retry policy demoted to
    // kStrictlyNotIdempotent, which would mask the retry-stopping behavior under test.
    txnRouter.attachTxnFieldsIfNeeded(
        operationContext(), kTestRemoteShardId1, BSON("find" << "bar"));
    txnRouter.attachTxnFieldsIfNeeded(
        operationContext(), kTestRemoteShardId2, BSON("find" << "bar"));

    // Idempotent retry policy: the sibling's retryable failure below would be retried unless the
    // metadata failure stopped retrying first.
    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                kTestNss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kIdempotent);

    // Record shard3's term (5) as if an earlier response announced it.
    txnRouter.processParticipantResponse(
        operationContext(),
        kTestRemoteShardId1,
        TransactionRouter::Router::parseParticipantResponseMetadata(
            BSON("ok" << 1 << "readOnly" << true << "additionalParticipants"
                      << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId3 << "readOnly" << true
                                                   << "term" << 5LL)))));

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(msars.next(), AssertionException, ErrorCodes::NoSuchTransaction);
    });

    {
        // Hold the drain's next() until the metadata failure has stopped retrying; otherwise the
        // sibling's failure can be processed, and its retry scheduled, before stopRetrying() runs.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            return BSON("ok" << 1 << "readOnly" << true << "additionalParticipants"
                             << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId3 << "readOnly"
                                                          << true << "term" << 6LL)));
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) -> StatusWith<BSONObj> {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    future.default_timed_get();
}

class ShardsvrMultiStatementTransactionRequestsSenderTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        // Populate the shardRegistry
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId1, kTestRemoteHostAndPort1));
        shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId2, kTestRemoteHostAndPort2));
        addRemoteShards(shardInfos);
    }

protected:
    void assertFailedToUnyieldError(Status status, ErrorCodes::Error expectedOriginalCode) {
        ASSERT_EQ(status.code(), ErrorCodes::TransactionParticipantFailedUnyield);

        auto originalErrorInfo = status.extraInfo<TransactionParticipantFailedUnyieldInfo>();
        auto originalStatus = originalErrorInfo->getOriginalError();
        ASSERT_EQ(originalStatus.code(), expectedOriginalCode);
    }
};

TEST_F(ShardsvrMultiStatementTransactionRequestsSenderTest,
       RequestsFailWithTransactionParticipantFailedUnyield) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();

    repl::ReadConcernArgs readConcernArgs{LogicalTime(Timestamp(1, 0)),
                                          repl::ReadConcernLevel::kMajorityReadConcern};
    repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;

    // Set up transaction state
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
    auto contextSession = mongoDSessionCatalog->checkOutSession(operationContext());

    auto txnParticipant = TransactionParticipant::get(operationContext());
    txnParticipant.beginOrContinue(operationContext(),
                                   TxnNumber(0),
                                   false,
                                   TransactionParticipant::TransactionActions::kStart);
    txnParticipant.unstashTransactionResources(operationContext(), "insert");

    // Schedule remote requests
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestRemoteShardId1, BSON("find" << "bar"));
    requests.emplace_back(kTestRemoteShardId2, BSON("find" << "bar"));
    std::set<ShardId> pendingShardIds{kTestRemoteShardId1, kTestRemoteShardId2};

    auto msars = MultiStatementTransactionRequestsSender(
        operationContext(),
        executor(),
        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kNoRetry);

    // Enable restoreLocksFail failpoint, which should cause unyielding to fail
    globalFailPointRegistry().find("restoreLocksFail")->setMode(FailPoint::alwaysOn);

    // Get the response from the first find request. Assert TransactionParticipantFailedUnyield is
    // thrown even after a successful response, and contains the original LockTimeout error.
    auto future = launchAsync([&]() {
        auto response = msars.next();
        auto status = response.swResponse.getStatus();
        ASSERT(response.shardId.isValid());
        pendingShardIds.erase(response.shardId);
        assertFailedToUnyieldError(status, ErrorCodes::LockTimeout);
    });

    // Mock the successful find response
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(
                   NamespaceString::createNamespaceString_forTest("db.bar"), 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();

    // Disable fail point
    globalFailPointRegistry().find("restoreLocksFail")->setMode(FailPoint::off);

    // Get the response from the second find request. It should have been marked as failed with the
    // same error, regardless of whether we received a response yet.
    future = launchAsync([&]() {
        while (!msars.done()) {
            auto response = msars.next();
            auto status = response.swResponse.getStatus();
            ASSERT(response.shardId.isValid());
            pendingShardIds.erase(response.shardId);
            assertFailedToUnyieldError(status, ErrorCodes::LockTimeout);
        }
    });

    future.default_timed_get();
    ASSERT(pendingShardIds.empty());

    SessionCatalog::get(operationContext()->getServiceContext())->reset_forTest();
}

TEST_F(ShardsvrMultiStatementTransactionRequestsSenderTest, FailUnyieldIfTxnStashDoesNotExist) {
    auto lsid = makeLogicalSessionIdForTest();
    operationContext()->setLogicalSessionId(lsid);
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();

    repl::ReadConcernArgs readConcernArgs{LogicalTime(Timestamp(1, 0)),
                                          repl::ReadConcernLevel::kMajorityReadConcern};
    repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;

    // Set up transaction state
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
    auto contextSession = mongoDSessionCatalog->checkOutSession(operationContext());

    auto txnParticipant = TransactionParticipant::get(operationContext());
    txnParticipant.beginOrContinue(operationContext(),
                                   TxnNumber(0),
                                   false,
                                   TransactionParticipant::TransactionActions::kStart);
    txnParticipant.unstashTransactionResources(operationContext(), "insert");

    // Schedule remote request
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestRemoteShardId1, BSON("find" << "bar"));
    std::set<ShardId> pendingShardIds{kTestRemoteShardId1};

    auto msars = MultiStatementTransactionRequestsSender(
        operationContext(),
        executor(),
        DatabaseName::createDatabaseName_forTest(boost::none, "db"),
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kNoRetry);

    // Force the ARS to hang after it yields
    setGlobalFailPoint("hangAfterYield", BSON("mode" << "alwaysOn"));

    // Call MSARS::next(), which will hang after yielding because of the failpoint above. Once the
    // failpoint is turned off and the MSARS tries to unyield, it should fail.
    auto errorOnMsarsNextFuture = launchAsync([&]() {
        auto response = msars.next();

        // Get the response from the find request. Assert TransactionParticipantFailedUnyield is
        // thrown even after a successful response, and contains an error with code 9183900.
        auto status = response.swResponse.getStatus();
        assertFailedToUnyieldError(status, ErrorCodes::duplicateCodeForTest(9183900));
    });

    // Mock a concurrent request in the same transaction. This request will block waiting to check
    // out the session until the MSARS yields. We don't actually run an op here, but instead set up
    // the TransactionParticipant state in such a way to mimic the request failing without aborting
    // the transaction. This will mean the request unstashes the txnResources, but does not restash
    // them nor mark the transaction as aborted.
    auto mockConcurrentRequestFuture = launchAsync([&]() {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();

        newOpCtx->setLogicalSessionId(lsid);
        newOpCtx->setTxnNumber(TxnNumber(0));
        newOpCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(newOpCtx.get());
        auto contextSession = mongoDSessionCatalog->checkOutSession(newOpCtx.get());

        auto txnParticipant = TransactionParticipant::get(newOpCtx.get());
        txnParticipant.beginOrContinue(newOpCtx.get(),
                                       TxnNumber(0),
                                       false,
                                       TransactionParticipant::TransactionActions::kContinue);
        txnParticipant.unstashTransactionResources(newOpCtx.get(), "insert");
    });

    // Wait until the concurrent request has unstashed the txnResources, then turn off the
    // failpoint.
    mockConcurrentRequestFuture.default_timed_get();
    setGlobalFailPoint("hangAfterYield", BSON("mode" << "off"));

    // Mock a successful find response from the remote shard, and then wait for the MSARS to fail
    // when unyieding.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(
                   NamespaceString::createNamespaceString_forTest("db.bar"), 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    errorOnMsarsNextFuture.default_timed_get();

    SessionCatalog::get(operationContext()->getServiceContext())->reset_forTest();
}

}  // namespace
}  // namespace mongo
