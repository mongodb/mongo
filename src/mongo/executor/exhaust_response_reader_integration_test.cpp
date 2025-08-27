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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/async_client.h"
#include "mongo/db/commands/kill_operations_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/executor/executor_integration_test_connection_stats.h"
#include "mongo/executor/exhaust_response_reader_tl.h"
#include "mongo/executor/pooled_async_client_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <memory>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/async_client_factory.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::executor {

class ExhaustResponseReaderIntegrationFixture : public unittest::Test {
public:
    void setUp() override {
        const auto instanceName = "ExhaustResponseReader";
        auto connectionString = unittest::getFixtureConnectionString();
        auto server = connectionString.getServers().front();

        auto sc = getGlobalServiceContext();
        auto tl = sc->getTransportLayerManager()->getDefaultEgressLayer();
        _reactor = tl->getReactor(transport::TransportLayer::kNewReactor);
        _reactorThread = stdx::thread([&] {
            _reactor->run();
            _reactor->drain();
        });

        if (!unittest::shouldUseGRPCEgress()) {
            ConnectionPool::Options connPoolOptions;
            connPoolOptions.minConnections = 0;
            _pool =
                std::make_shared<PooledAsyncClientFactory>(instanceName, connPoolOptions, nullptr);
        } else {
#ifdef MONGO_CONFIG_GRPC
            _pool = std::make_shared<transport::grpc::GRPCAsyncClientFactory>(instanceName);
#else
            MONGO_UNREACHABLE;
#endif
        }
        _pool->startup(sc, tl, _reactor);
    }

    void tearDown() override {
        _pool->shutdown();
        _reactor->stop();
        _reactorThread.join();
    }

    RemoteCommandResponse assertOK(StatusWith<RemoteCommandResponse> swResp) {
        ASSERT_OK(swResp);
        ASSERT_OK(swResp.getValue().status);
        ASSERT_OK(getStatusFromCommandResult(swResp.getValue().data));
        return swResp.getValue();
    }

    HostAndPort getServer() {
        return unittest::getFixtureConnectionString().getServers().front();
    }

    std::shared_ptr<AsyncClientFactory::AsyncClientHandle> getConnection() {
        return _pool->get(getServer(), transport::ConnectSSLMode::kGlobalSSLMode, Seconds(30))
            .get();
    }

    AsyncClientFactory& getPool() {
        return *_pool;
    }

    AsyncDBClient* getClient(const ConnectionPool::ConnectionHandle& conn) {
        return checked_cast<connection_pool_tl::TLConnection*>(conn.get())->client();
    }

    RemoteCommandRequest makeTestRequest(DatabaseName dbName,
                                         BSONObj cmdObj,
                                         boost::optional<UUID> clientOperationKey = boost::none,
                                         Milliseconds timeout = RemoteCommandRequest::kNoTimeout) {
        if (clientOperationKey) {
            cmdObj = cmdObj.addField(
                BSON(GenericArguments::kClientOperationKeyFieldName << *clientOperationKey)
                    .firstElement());
        }

        return RemoteCommandRequest(getServer(),
                                    std::move(dbName),
                                    std::move(cmdObj),
                                    BSONObj(),
                                    nullptr,
                                    timeout,
                                    {},
                                    clientOperationKey);
    }

    RemoteCommandRequest makeExhaustHello(Milliseconds maxAwaitTime,
                                          boost::optional<UUID> clientOperationKey = boost::none) {
        auto helloCmd =
            BSON("hello" << 1 << "maxAwaitTimeMS" << maxAwaitTime.count() << "topologyVersion"
                         << TopologyVersion(OID::max(), 0).toBSON());
        return makeTestRequest(DatabaseName::kAdmin, std::move(helloCmd), clientOperationKey);
    }

    std::shared_ptr<transport::Reactor> getReactor() {
        return _reactor;
    }

    RemoteCommandResponse assertReadOK(NetworkInterface::ExhaustResponseReader& rdr) {
        auto resp = rdr.next().get();
        LOGV2(9311390, "Received exhaust response", "response"_attr = resp.toString());
        return assertOK(resp);
    }

private:
    std::shared_ptr<AsyncClientFactory> _pool;
    stdx::thread _reactorThread;
    std::shared_ptr<transport::Reactor> _reactor;
};

TEST_F(ExhaustResponseReaderIntegrationFixture, ReceiveMultipleResponses) {
    CancellationSource cancelSource;

    auto conn = getConnection();
    auto& client = conn->getClient();

    auto request = makeExhaustHello(Milliseconds(150));
    auto rdr = std::make_shared<ExhaustResponseReaderTL>(
        request,
        assertOK(client.beginExhaustCommandRequest(request).getNoThrow()),
        std::move(conn),
        nullptr,
        getReactor(),
        cancelSource.token());

    for (int i = 0; i < 3; i++) {
        auto next = assertReadOK(*rdr);
        ASSERT(next.moreToCome);
    }

    cancelSource.cancel();

    ASSERT_EQ(rdr->next().get().status, ErrorCodes::CallbackCanceled);
    ASSERT_EQ(rdr->next().get().status, ErrorCodes::ExhaustCommandFinished);

    rdr.reset();
    assertConnectionStatsSoon(
        getPool(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        [&](const GRPCConnectionStats& stats) {
            return stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() == 0;
        },
        "ReceiveMultipleResponses test timed out while waiting for connection count to drop to 0.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, CancelRead) {
    CancellationSource cancelSource;

    auto conn = getConnection();
    auto& client = conn->getClient();

    // Use longer maxAwaitTimeMS to ensure we start waiting.
    auto request = makeExhaustHello(Seconds(30));
    request.timeout = Seconds(15);

    auto rdr = std::make_shared<ExhaustResponseReaderTL>(
        request,
        assertOK(client.beginExhaustCommandRequest(request).getNoThrow()),
        std::move(conn),
        nullptr,
        getReactor(),
        cancelSource.token());

    // First response is retrieved from the initial command.
    assertReadOK(*rdr);

    // Subsequent responses will come every maxAwaitTimeMS.
    auto respFut = rdr->next();
    cancelSource.cancel();

    // next() always returns an OK future, but the interior status may be non-OK.
    auto cancelled = respFut.get();
    ASSERT_EQ(cancelled.status, ErrorCodes::CallbackCanceled);

    rdr.reset();
    assertConnectionStatsSoon(
        getPool(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        [&](const GRPCConnectionStats& stats) {
            return stats.getTotalInUseStreams() + stats.getTotalLeasedStreams() == 0;
        },
        "CancelRead test timed out while waiting for connection count to drop to 0.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, CommandSucceeds) {
    std::vector<BSONObj> documents = {
        BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)};

    const auto nss = NamespaceStringUtil::deserialize(DatabaseName::kMdbTesting, "CommandSucceeds");

    auto conn = getConnection();
    auto& client = conn->getClient();

    assertOK(client.runCommandRequest(makeTestRequest(nss.dbName(), BSON("drop" << nss.coll())))
                 .getNoThrow());

    write_ops::InsertCommandRequest insert(nss);
    insert.setDocuments(documents);
    assertOK(client.runCommandRequest(makeTestRequest(nss.dbName(), insert.toBSON())).getNoThrow());

    FindCommandRequest find(nss);
    find.setSort(BSON("_id" << 1));
    find.setBatchSize(0);
    auto findResp = assertOK(
        client.runCommandRequest(makeTestRequest(nss.dbName(), find.toBSON())).getNoThrow());
    auto cursorReply = CursorInitialReply::parse(findResp.data, IDLParserContext("findReply"));

    GetMoreCommandRequest getMore(cursorReply.getCursor()->getCursorId(), std::string{nss.coll()});
    getMore.setDbName(nss.dbName());
    getMore.setBatchSize(1);
    auto getMoreRequest = makeTestRequest(nss.dbName(), getMore.toBSON());
    auto rdr = std::make_shared<ExhaustResponseReaderTL>(
        getMoreRequest,
        assertOK(client.beginExhaustCommandRequest(getMoreRequest).getNoThrow()),
        std::move(conn),
        nullptr,
        getReactor());

    for (size_t i = 0; i < documents.size(); i++) {
        auto& expected = documents[i];

        auto resp = assertReadOK(*rdr);
        auto parsed = CursorGetMoreReply::parse(resp.data, IDLParserContext("CursorGetMoreReply"));
        auto batch = parsed.getCursor().getNextBatch();
        ASSERT_EQ(batch.size(), 1);
        ASSERT_BSONOBJ_EQ(batch[0], expected);

        ASSERT(resp.moreToCome);
    }

    // We should have exhausted all documents and received a final empty batch.
    auto last = assertReadOK(*rdr);
    auto parsed = CursorGetMoreReply::parse(last.data, IDLParserContext("CursorGetMoreReply"));
    auto batch = parsed.getCursor().getNextBatch();
    ASSERT_EQ(batch.size(), 0);
    ASSERT_FALSE(last.moreToCome);

    auto after = rdr->next().get();
    ASSERT_EQ(after.status, ErrorCodes::ExhaustCommandFinished);
    ASSERT_FALSE(after.moreToCome);

    rdr.reset();

    assertConnectionStatsSoon(
        getPool(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            // Once command has completed, the connection should be returned to the pool
            return stats.inUse == 0 && stats.available == 1;
        },
        [&](const GRPCConnectionStats& stats) {
            // There should still be one open channel.
            return stats.getTotalInUseStreams() == 0 && stats.getTotalOpenChannels() == 1;
        },
        "CommandSucceeds test timed out while waiting for 0 inUse connections and 1 available/open "
        "connection.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, RemoteCancel) {
    auto conn = getConnection();
    auto& client = conn->getClient();

    // Use long maxAwaitTimeMS to ensure we start waiting.
    auto clientOperationKey = UUID::gen();
    auto request = makeExhaustHello(Seconds(60), clientOperationKey);
    request.timeout = Seconds(30);
    auto rdr = std::make_shared<ExhaustResponseReaderTL>(
        request,
        assertOK(client.beginExhaustCommandRequest(request).getNoThrow()),
        std::move(conn),
        nullptr,
        getReactor());

    // First response will be available immediately from the initial command.
    assertReadOK(*rdr);

    // The next one will not be received until maxAwaitTimeMS is hit.
    auto respFut = rdr->next();

    LOGV2(9311391,
          "Killing exhaust command remotely",
          "clientOperationKey"_attr = clientOperationKey);
    auto cancelConn = getConnection();
    auto& cancelClient = cancelConn->getClient();
    KillOperationsRequest killOps({clientOperationKey});
    killOps.setDbName(DatabaseName::kAdmin);
    assertOK(cancelClient.runCommandRequest(makeTestRequest(DatabaseName::kAdmin, killOps.toBSON()))
                 .getNoThrow());
    cancelConn->indicateUsed();
    cancelConn->indicateSuccess();
    cancelConn.reset();

    auto resp = respFut.get();
    LOGV2(9311392, "Received response after cancellation", "response"_attr = resp.toString());
    ASSERT_OK(resp.status);
    ASSERT_NOT_OK(getStatusFromCommandResult(resp.data));

    rdr.reset();
    assertConnectionStatsSoon(
        getPool(),
        getServer(),
        [](const ConnectionStatsPer& stats) {
            // connection should be returned to the pool after graceful cancellation
            return stats.inUse == 0 && stats.available == 2;
        },
        [&](const GRPCConnectionStats& stats) {
            // There should still be one open channel.
            return stats.getTotalInUseStreams() == 0 && stats.getTotalOpenChannels() == 1;
        },
        "RemoteCancel test timed out while waiting for 0 inUse connections and the correct number "
        "of available/open connections");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, LocalTimeout) {
    auto conn = getConnection();
    auto& client = conn->getClient();

    // Use a maxAwaitTimeMS longer than the local timeout.
    auto request = makeExhaustHello(Milliseconds(60000));
    request.timeout = Milliseconds(500);

    auto rdr = std::make_shared<ExhaustResponseReaderTL>(
        request,
        assertOK(client.beginExhaustCommandRequest(request).getNoThrow()),
        std::move(conn),
        nullptr,
        getReactor());

    // The initial response wil be available immediately.
    assertReadOK(*rdr);

    // The next one will not be received until maxAwaitTimeMS is hit, so the 500ms local timeout
    // will cancel the operation early.
    auto resp = rdr->next().get();
    ASSERT_EQ(resp.status, ErrorCodes::CallbackCanceled);
    rdr.reset();
    assertConnectionStatsSoon(
        getPool(),
        getServer(),
        [](const ConnectionStatsPer& stats) { return stats.inUse == 0 && stats.available == 0; },
        [&](const GRPCConnectionStats& stats) {
            // There should still be one open channel.
            return stats.getTotalInUseStreams() == 0 && stats.getTotalOpenChannels() == 1;
        },
        "LocalTimeout test timed out while waiting for 0 inUse connections and the correct number "
        "of available/open connections.");
}

}  // namespace mongo::executor
