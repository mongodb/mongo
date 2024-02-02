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
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

class MultiStatementTransactionRequestsSenderTest
    : public virtual service_context_test::RouterRoleOverride,
      public ShardingTestFixture {
public:
    MultiStatementTransactionRequestsSenderTest() {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(HostAndPort("FakeConfigHost", 12345));

        std::vector<ShardType> shards;
        ShardType shardType;
        shardType.setName(_remoteShardId.toString());
        shardType.setHost(_remoteHostAndPort.toString());
        shards.push_back(shardType);

        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            std::make_unique<RemoteCommandTargeterMock>());
        _targeters.push_back(targeter.get());
        targeter->setConnectionStringReturnValue(ConnectionString(_remoteHostAndPort));
        targeter->setFindHostReturnValue(_remoteHostAndPort);
        targeterFactory()->addTargeterToReturn(ConnectionString(_remoteHostAndPort),
                                               std::move(targeter));

        setupShards(shards);
    }

protected:
    void checkRequestMetadata(executor::RemoteCommandRequest request,
                              StringData expectedCmd,
                              bool expectTxnFields) {
        ASSERT(request.cmdObj.hasField(expectedCmd));
        if (expectTxnFields) {
            ASSERT(request.cmdObj.hasField("startTransaction"));
            ASSERT(request.cmdObj.hasField("autocommit"));
            ASSERT(request.cmdObj.hasField("txnNumber"));
            return;
        }

        ASSERT(!request.cmdObj.hasField("startTransaction"));
        ASSERT(!request.cmdObj.hasField("autocommit"));
        ASSERT(!request.cmdObj.hasField("txnNumber"));
    }

    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.bar");
    const ShardId _remoteShardId = ShardId("FakeShard1");
    const HostAndPort _remoteHostAndPort = HostAndPort("FakeShard1Host", 12345);
    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
};

TEST_F(MultiStatementTransactionRequestsSenderTest, TxnDetailsNotAppendedIfNoTxnRouter) {
    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{{_remoteShardId, BSON(cmdName << "bar")}};

    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                _nss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    auto future = launchAsync([&]() {
        auto response = msars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    onCommand([&](const auto& request) {
        checkRequestMetadata(request, cmdName, false /* expectTxnFields */);
        return BSON("ok" << true);
    });

    future.default_timed_get();
}


TEST_F(MultiStatementTransactionRequestsSenderTest, TxnDetailsAreAppendedIfTxnRouter) {
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(0));
    operationContext()->setInMultiDocumentTransaction();
    RouterOperationContextSession rocs(operationContext());
    TransactionRouter::get(operationContext())
        .beginOrContinueTxn(
            operationContext(), TxnNumber(0), TransactionRouter::TransactionActions::kStart);
    TransactionRouter::get(operationContext()).setDefaultAtClusterTime(operationContext());

    auto cmdName = "find";
    std::vector<AsyncRequestsSender::Request> requests{{_remoteShardId,
                                                        BSON("find"
                                                             << "bar")}};

    auto msars =
        MultiStatementTransactionRequestsSender(operationContext(),
                                                executor(),
                                                _nss.dbName(),
                                                requests,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                Shard::RetryPolicy::kNoRetry);

    auto future = launchAsync([&]() {
        auto response = msars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    onCommand([&](const auto& request) {
        checkRequestMetadata(request, cmdName, true /* expectTxnFields */);

        // The TransactionRouter will throw when parsing this response if
        // "processParticipantResponse" does not exist
        return BSON("ok" << true << "readOnly" << true);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
