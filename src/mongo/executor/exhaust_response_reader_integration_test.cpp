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

#include "mongo/executor/exhaust_response_reader_tl.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/async_client.h"
#include "mongo/db/commands/kill_operations_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/query/cursor_response_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::executor {

class ExhaustResponseReaderIntegrationFixture : public unittest::Test {
public:
    void setUp() override {
        auto connectionString = unittest::getFixtureConnectionString();
        auto server = connectionString.getServers().front();

        auto sc = getGlobalServiceContext();
        auto tl = sc->getTransportLayerManager()->getEgressLayer();
        _reactor = tl->getReactor(transport::TransportLayer::kNewReactor);
        _reactorThread = stdx::thread([&] { _reactor->run(); });

        ConnectionPool::Options connPoolOptions;
        connPoolOptions.minConnections = 0;
        auto typeFactory = std::make_unique<connection_pool_tl::TLTypeFactory>(
            _reactor, sc->getTransportLayerManager(), nullptr, connPoolOptions, nullptr);
        _pool = std::make_shared<ConnectionPool>(
            std::move(typeFactory), "ExhaustResponseReader", connPoolOptions);
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

    ConnectionPool::ConnectionHandle getConnection() {
        return _pool->get(getServer(), transport::ConnectSSLMode::kGlobalSSLMode, Seconds(30))
            .get();
    }

    ConnectionPool& getPool() {
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

    ConnectionStatsPer getPoolStats() {
        ConnectionPoolStats stats;
        getPool().appendConnectionStats(&stats);
        return stats.statsByHost[getServer()];
    }

    RemoteCommandResponse assertReadOK(NetworkInterface::ExhaustResponseReader& rdr) {
        auto resp = rdr.next().get();
        LOGV2(9311390, "Received exhaust response", "response"_attr = resp.toString());
        return assertOK(resp);
    }

    /*
     * Asserts that the connection pool stats reach a certain value within a 30 second window. The
     * connection pool stats to test and the value they must reach are defined in f.
     * TODO: SERVER-66126 We should be able to test directly without any wait once continuation get
     * destructed right after they run.
     */
    void assertConnectionStatsSoon(std::function<bool(const ConnectionStatsPer&)> f,
                                   StringData errMsg) {
        auto start = getGlobalServiceContext()->getFastClockSource()->now();
        while (getGlobalServiceContext()->getFastClockSource()->now() - start < Seconds(30)) {
            if (f(getPoolStats())) {
                return;
            }
            sleepFor(Milliseconds(100));
        }
        FAIL(errMsg.toString());
    }

private:
    std::shared_ptr<ConnectionPool> _pool;
    stdx::thread _reactorThread;
    std::shared_ptr<transport::Reactor> _reactor;
};

TEST_F(ExhaustResponseReaderIntegrationFixture, ReceiveMultipleResponses) {
    CancellationSource cancelSource;

    auto conn = getConnection();
    auto client = getClient(conn);

    auto request = makeExhaustHello(Milliseconds(150));
    assertOK(client->beginExhaustCommandRequest(request).getNoThrow());
    auto rdr = ExhaustResponseReaderTL::make_forTest(
        request, std::move(conn), nullptr, getReactor(), cancelSource.token());

    for (int i = 0; i < 3; i++) {
        auto next = assertReadOK(*rdr);
        ASSERT(next.moreToCome);
    }

    cancelSource.cancel();

    ASSERT_EQ(rdr->next().get().status, ErrorCodes::CallbackCanceled);
    ASSERT_EQ(rdr->next().get().status, ErrorCodes::ExhaustCommandFinished);

    rdr.reset();
    assertConnectionStatsSoon(
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        "ReceiveMultipleResponses test timed out while waiting for connection count to drop to 0.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, CancelRead) {
    CancellationSource cancelSource;

    auto conn = getConnection();
    auto client = getClient(conn);

    // Use longer maxAwaitTimeMS to ensure we start waiting.
    auto request = makeExhaustHello(Seconds(30));
    request.timeout = Seconds(15);

    assertOK(client->beginExhaustCommandRequest(request).getNoThrow());
    auto rdr = ExhaustResponseReaderTL::make_forTest(
        request, std::move(conn), nullptr, getReactor(), cancelSource.token());

    // Subsequent responses will come every maxAwaitTimeMS.
    auto respFut = rdr->next();
    cancelSource.cancel();

    // next() always returns an OK future, but the interior status may be non-OK.
    auto cancelled = respFut.get();
    ASSERT_EQ(cancelled.status, ErrorCodes::CallbackCanceled);

    rdr.reset();
    assertConnectionStatsSoon(
        [](const ConnectionStatsPer& stats) {
            return stats.inUse + stats.available + stats.leased == 0;
        },
        "CancelRead test timed out while waiting for connection count to drop to 0.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, CommandSucceeds) {
    std::vector<BSONObj> documents = {
        BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)};

    const auto nss = NamespaceStringUtil::deserialize(DatabaseName::kMdbTesting, "CommandSucceeds");

    auto conn = getConnection();
    auto client = getClient(conn);

    assertOK(client->runCommandRequest(makeTestRequest(nss.dbName(), BSON("drop" << nss.coll())))
                 .getNoThrow());

    write_ops::InsertCommandRequest insert(nss);
    insert.setDocuments(documents);
    assertOK(
        client->runCommandRequest(makeTestRequest(nss.dbName(), insert.toBSON())).getNoThrow());

    FindCommandRequest find(nss);
    find.setSort(BSON("_id" << 1));
    find.setBatchSize(1);
    auto findResp = assertOK(
        client->runCommandRequest(makeTestRequest(nss.dbName(), find.toBSON())).getNoThrow());
    auto cursorReply = CursorInitialReply::parse(IDLParserContext("findReply"), findResp.data);

    GetMoreCommandRequest getMore(cursorReply.getCursor()->getCursorId(), nss.coll().toString());
    getMore.setDbName(nss.dbName());
    getMore.setBatchSize(1);
    auto getMoreRequest = makeTestRequest(nss.dbName(), getMore.toBSON());
    assertOK(client->beginExhaustCommandRequest(getMoreRequest).getNoThrow());
    auto rdr = ExhaustResponseReaderTL::make_forTest(
        getMoreRequest, std::move(conn), nullptr, getReactor());

    // Skip one for the find request and one for the initial getMore.
    for (size_t i = 2; i < documents.size(); i++) {
        auto& expected = documents[i];

        auto resp = assertReadOK(*rdr);
        auto parsed = CursorGetMoreReply::parse(IDLParserContext("CursorGetMoreReply"), resp.data);
        auto batch = parsed.getCursor().getNextBatch();
        ASSERT_EQ(batch.size(), 1);
        ASSERT_BSONOBJ_EQ(batch[0], expected);

        ASSERT(resp.moreToCome);
    }

    // We should have exhausted all documents and received a final empty batch.
    auto last = assertReadOK(*rdr);
    auto parsed = CursorGetMoreReply::parse(IDLParserContext("CursorGetMoreReply"), last.data);
    auto batch = parsed.getCursor().getNextBatch();
    ASSERT_EQ(batch.size(), 0);
    ASSERT_FALSE(last.moreToCome);

    auto after = rdr->next().get();
    ASSERT_EQ(after.status, ErrorCodes::ExhaustCommandFinished);
    ASSERT_FALSE(after.moreToCome);

    rdr.reset();

    // Once command has completed, the connection should be returned to the pool.
    assertConnectionStatsSoon(
        [](const ConnectionStatsPer& stats) { return stats.available == 1; },
        "CommandSucceeds test timed out while waiting for the number of available "
        "connections to reach 1.");
    assertConnectionStatsSoon([](const ConnectionStatsPer& stats) { return stats.inUse == 0; },
                              "CommandSucceeds test timed out while waiting for the number of in "
                              "use connection to reach 0.");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, RemoteCancel) {
    auto conn = getConnection();
    auto client = getClient(conn);

    // Use long maxAwaitTimeMS to ensure we start waiting.
    auto clientOperationKey = UUID::gen();
    auto request = makeExhaustHello(Seconds(60), clientOperationKey);
    request.timeout = Seconds(30);
    assertOK(client->beginExhaustCommandRequest(request).getNoThrow());
    auto rdr =
        ExhaustResponseReaderTL::make_forTest(request, std::move(conn), nullptr, getReactor());

    // The next one will not be received until maxAwaitTimeMS is hit.
    auto respFut = rdr->next();

    LOGV2(9311391,
          "Killing exhaust command remotely",
          "clientOperationKey"_attr = clientOperationKey);
    auto cancelConn = getConnection();
    auto cancelClient = getClient(cancelConn);
    KillOperationsRequest killOps({clientOperationKey});
    killOps.setDbName(DatabaseName::kAdmin);
    assertOK(
        cancelClient->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, killOps.toBSON()))
            .getNoThrow());
    cancelConn->indicateUsed();
    cancelConn->indicateSuccess();

    auto resp = respFut.get();
    LOGV2(9311392, "Received response after cancellation", "response"_attr = resp.toString());
    ASSERT_OK(resp.status);
    ASSERT_NOT_OK(getStatusFromCommandResult(resp.data));

    rdr.reset();
    assertConnectionStatsSoon(
        [](const ConnectionStatsPer& stats) { return stats.available == 1; },
        "connection should be returned to the pool after graceful cancellation");
}

TEST_F(ExhaustResponseReaderIntegrationFixture, LocalTimeout) {
    auto conn = getConnection();
    auto client = getClient(conn);

    // Use a maxAwaitTimeMS longer than the local timeout.
    auto request = makeExhaustHello(Milliseconds(60000));
    request.timeout = Milliseconds(500);

    assertOK(client->beginExhaustCommandRequest(request).getNoThrow());
    auto rdr =
        ExhaustResponseReaderTL::make_forTest(request, std::move(conn), nullptr, getReactor());

    // The next one will not be received until maxAwaitTimeMS is hit, so the 500ms local timeout
    // will cancel the operation early.
    auto resp = rdr->next().get();
    ASSERT_EQ(resp.status, ErrorCodes::CallbackCanceled);
    assertConnectionStatsSoon([](const ConnectionStatsPer& stats) { return stats.available == 0; },
                              "LocalTimeout test timed out while waiting for the number of "
                              "available connection to reach 0.");
}

}  // namespace mongo::executor
