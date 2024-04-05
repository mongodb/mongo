/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/net/hostandport.h"
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <system_error>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("test.bar");
const ShardId kTestRemoteShardId = ShardId("FakeShard1");
const HostAndPort kTestRemoteHostAndPort = HostAndPort("FakeShard1Host", 12345);

class RouterMultiStatementTransactionRequestsSenderTest
    : public virtual service_context_test::RouterRoleOverride,
      public ShardingTestFixture {
public:
    RouterMultiStatementTransactionRequestsSenderTest() {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(HostAndPort("FakeConfigHost", 12345));

        std::vector<ShardType> shards;
        ShardType shardType;
        shardType.setName(kTestRemoteShardId.toString());
        shardType.setHost(kTestRemoteHostAndPort.toString());
        shards.push_back(shardType);

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        _targeters.push_back(targeter.get());
        targeter->setConnectionStringReturnValue(ConnectionString(kTestRemoteHostAndPort));
        targeter->setFindHostReturnValue(kTestRemoteHostAndPort);
        targeterFactory()->addTargeterToReturn(ConnectionString(kTestRemoteHostAndPort),
                                               std::move(targeter));

        setupShards(shards);
    }

protected:
    void checkRequestMetadata(executor::RemoteCommandRequest request,
                              StringData expectedCmd,
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

    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
};

TEST_F(RouterMultiStatementTransactionRequestsSenderTest, TxnDetailsNotAppendedIfNoTxnRouter) {
    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{
        {kTestRemoteShardId, BSON(cmdName << "bar")}};

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
                             false /* expectStartTxnFields */,
                             false /* expectStartOrContinueTxnFields */);
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
    std::vector<AsyncRequestsSender::Request> requests{{kTestRemoteShardId,
                                                        BSON("find"
                                                             << "bar")}};

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
                             true /* expectStartTxnFields */,
                             false /* expectStartOrContinueTxnFields */);

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
    ASSERT_OK(
        readConcernArgs.initialize(BSON("find"
                                        << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                                        << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                                                << LogicalTime(Timestamp(1, 0)).asTimestamp()
                                                << repl::ReadConcernArgs::kLevelFieldName
                                                << "snapshot"))));
    repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    RouterOperationContextSession rocs(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(operationContext(),
                            TxnNumber(0),
                            TransactionRouter::TransactionActions::kStartOrContinue);

    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{{kTestRemoteShardId,
                                                        BSON("find"
                                                             << "bar")}};

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
                             false /* expectStartTxnFields */,
                             true /* expectStartOrContinueTxnFields */);

        // The TransactionRouter will throw when parsing this response if
        // "processParticipantResponse" does not exist
        return BSON("ok" << true << "readOnly" << true);
    });

    future.default_timed_get();
}

class ShardsvrMultiStatementTransactionRequestsSenderTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(kTestRemoteHostAndPort));
        targeter->setFindHostReturnValue(kTestRemoteHostAndPort);
        targeterFactory()->addTargeterToReturn(ConnectionString(kTestRemoteHostAndPort),
                                               std::move(targeter));

        auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });
        onCommand([&](const auto& request) {
            ASSERT(request.cmdObj["find"]);
            const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
                request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss, NamespaceString::kConfigsvrShardsNamespace);

            ShardType shardType;
            shardType.setName(kTestRemoteShardId.toString());
            shardType.setHost(kTestRemoteHostAndPort.toString());
            return CursorResponse(
                       NamespaceString::kConfigsvrShardsNamespace, 0LL, {shardType.toBSON()})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });

        future.default_timed_get();
    }

protected:
    void assertFailedToUnyieldError(Status status, ErrorCodes::Error expectedOriginalCode) {
        ASSERT_EQ(status.code(), ErrorCodes::TransactionParticipantFailedUnyield);

        auto originalErrorInfo = status.extraInfo<TransactionParticipantFailedUnyieldInfo>();
        auto originalStatus = originalErrorInfo->getOriginalError();
        ASSERT_EQ(originalStatus.code(), expectedOriginalCode);
    }
    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
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
    requests.emplace_back(kTestRemoteShardId,
                          BSON("find"
                               << "bar"));
    requests.emplace_back(kTestRemoteShardId,
                          BSON("find"
                               << "bar"));

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
            assertFailedToUnyieldError(status, ErrorCodes::LockTimeout);
        }
    });

    future.default_timed_get();

    SessionCatalog::get(operationContext()->getServiceContext())->reset_forTest();
}

}  // namespace
}  // namespace mongo
