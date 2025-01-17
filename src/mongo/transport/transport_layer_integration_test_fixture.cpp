/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/transport/transport_layer_integration_test_fixture.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::transport {

std::shared_ptr<AsyncDBClient> AsyncClientIntegrationTestFixture::makeClient() {
    auto svcCtx = getServiceContext();
    auto metrics = std::make_shared<ConnectionMetrics>(svcCtx->getFastClockSource());
    auto client =
        AsyncDBClient::connect(getServer(),
                               transport::kGlobalSSLMode,
                               svcCtx,
                               svcCtx->getTransportLayerManager()->getDefaultEgressLayer(),
                               _reactor,
                               Milliseconds::max(),
                               metrics)
            .get();
    client->initWireVersion(__FILE__, nullptr).get();
    return client;
}

executor::RemoteCommandRequest AsyncClientIntegrationTestFixture::makeTestRequest(
    DatabaseName dbName,
    BSONObj cmdObj,
    boost::optional<UUID> clientOperationKey,
    Milliseconds timeout) {
    if (clientOperationKey) {
        cmdObj = cmdObj.addField(
            BSON(GenericArguments::kClientOperationKeyFieldName << *clientOperationKey)
                .firstElement());
    }

    return executor::RemoteCommandRequest(getServer(),
                                          std::move(dbName),
                                          std::move(cmdObj),
                                          BSONObj(),
                                          nullptr,
                                          timeout,
                                          {},
                                          clientOperationKey);
}

executor::RemoteCommandRequest AsyncClientIntegrationTestFixture::makeExhaustHello(
    Milliseconds maxAwaitTime, boost::optional<UUID> clientOperationKey) {
    // Send a dummy topologyVersion because the mongod generates this and sends it to the client
    // on the initial handshake.
    auto helloCmd = BSON("hello" << 1 << "maxAwaitTimeMS" << maxAwaitTime.count()
                                 << "topologyVersion" << TopologyVersion(OID::max(), 0).toBSON());
    return makeTestRequest(DatabaseName::kAdmin, std::move(helloCmd), clientOperationKey);
}

AsyncClientIntegrationTestFixture::FailPointGuard
AsyncClientIntegrationTestFixture::configureFailPoint(const std::shared_ptr<AsyncDBClient>& client,
                                                      StringData fp,
                                                      BSONObj data) {
    auto configureFailPointRequest = makeTestRequest(DatabaseName::kAdmin,
                                                     BSON("configureFailPoint" << fp << "mode"
                                                                               << "alwaysOn"
                                                                               << "data" << data));
    auto resp = assertOK(client->runCommandRequest(configureFailPointRequest).get());
    return FailPointGuard(fp, client, resp.getField("count").Int());
}

AsyncClientIntegrationTestFixture::FailPointGuard
AsyncClientIntegrationTestFixture::configureFailCommand(
    const std::shared_ptr<AsyncDBClient>& client,
    StringData failCommand,
    boost::optional<ErrorCodes::Error> errorCode,
    boost::optional<Milliseconds> blockTime) {
    auto data = BSON("failCommands" << BSON_ARRAY(failCommand));

    if (errorCode) {
        data = data.addField(BSON("errorCode" << *errorCode).firstElement());
    }

    if (blockTime) {
        data =
            data.addFields(BSON("blockConnection" << true << "blockTimeMS" << blockTime->count()));
    }
    return configureFailPoint(client, "failCommand", data);
}

void AsyncClientIntegrationTestFixture::killOp(AsyncDBClient& client, UUID opKey) {
    auto killOp = makeTestRequest(
        DatabaseName::kAdmin, BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(opKey)));
    assertOK(client.runCommandRequest(killOp).get());
}

// This test forces reads and writes to occur one byte at a time, verifying SERVER-34506
// (the isJustForContinuation optimization works).
//
// Because of the file size limit, it's only an effective check on debug builds (where the
// future implementation checks the length of the future chain).
void AsyncClientIntegrationTestFixture::testShortReadsAndWritesWork() {
    auto dbClient = makeClient();

    FailPointEnableBlock fp("asioTransportLayerShortOpportunisticReadWrite");

    auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1));
    assertOK(dbClient->runCommandRequest(req, baton()).get(interruptible()));
}

void AsyncClientIntegrationTestFixture::testAsyncConnectTimeoutCleansUpSocket() {
    FailPointEnableBlock fp("asioTransportLayerAsyncConnectTimesOut");
    auto svcCtx = getServiceContext();
    auto metrics = std::make_shared<ConnectionMetrics>(svcCtx->getFastClockSource());
    auto client =
        AsyncDBClient::connect(getServer(),
                               transport::kGlobalSSLMode,
                               svcCtx,
                               svcCtx->getTransportLayerManager()->getDefaultEgressLayer(),
                               getReactor(),
                               Milliseconds{500},
                               metrics)
            .getNoThrow();
    ASSERT_EQ(client.getStatus(), ErrorCodes::NetworkTimeout);
}

void AsyncClientIntegrationTestFixture::testExhaustHelloShouldReceiveMultipleReplies() {
    auto client = makeClient();

    auto req = makeExhaustHello(Milliseconds(100));

    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(req, baton());

    Date_t prevTime;
    TopologyVersion topologyVersion;
    {
        auto reply = beginExhaustFuture.get(interruptible());
        ASSERT(reply.moreToCome);
        assertOK(reply);
        prevTime = reply.data.getField("localTime").Date();
        topologyVersion = TopologyVersion::parse(IDLParserContext("TopologyVersion"),
                                                 reply.data.getField("topologyVersion").Obj());
    }

    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton());
    {
        auto reply = awaitExhaustFuture.get(interruptible());
        assertOK(reply);
        ASSERT(reply.moreToCome);
        auto replyTime = reply.data.getField("localTime").Date();
        ASSERT_GT(replyTime, prevTime);

        auto replyTopologyVersion = TopologyVersion::parse(
            IDLParserContext("TopologyVersion"), reply.data.getField("topologyVersion").Obj());
        ASSERT_EQ(replyTopologyVersion.getProcessId(), topologyVersion.getProcessId());
        ASSERT_EQ(replyTopologyVersion.getCounter(), topologyVersion.getCounter());
    }

    Future<executor::RemoteCommandResponse> cancelExhaustFuture =
        client->awaitExhaustCommand(baton());
    {
        client->cancel();
        client->end();
        auto swReply = cancelExhaustFuture.getNoThrow(interruptible());

        // The original hello request has maxAwaitTimeMs = 1000 ms, if the cancel executes
        // before the 1000ms then we expect the future to resolve with an error. It should
        // resolve with CallbackCanceled unless the socket is already closed, in which case
        // it will resolve with HostUnreachable. If the network is slow, the server may
        // response before the cancel executes however.
        if (!swReply.getStatus().isOK()) {
            ASSERT((swReply.getStatus() == ErrorCodes::CallbackCanceled) ||
                   (swReply.getStatus() == ErrorCodes::HostUnreachable));
        }
    }
}

void AsyncClientIntegrationTestFixture::testExhaustHelloShouldStopOnFailure() {
    auto client = makeClient();
    auto fpClient = makeClient();

    // Turn on the failCommand fail point for hello
    auto fpGuard = configureFailCommand(fpClient, "hello", ErrorCodes::CommandFailed);

    auto helloReq = makeExhaustHello(Seconds(1));

    auto resp = client->beginExhaustCommandRequest(helloReq, baton()).get(interruptible());
    ASSERT_OK(resp.status);
    ASSERT(!resp.moreToCome);
    ASSERT_EQ(getStatusFromCommandResult(resp.data), ErrorCodes::CommandFailed);
}

void AsyncClientIntegrationTestFixture::testRunCommandRequest() {
    auto client = makeClient();
    auto req = makeTestRequest(DatabaseName::kAdmin, BSON("echo" << std::string(1 << 10, 'x')));
    assertOK(client->runCommandRequest(req, baton()).get(interruptible()));
}

void AsyncClientIntegrationTestFixture::testRunCommandRequestCancel() {
    auto fpClient = makeClient();
    CancellationSource cancelSource;

    auto fpGuard = configureFailCommand(fpClient, "ping", boost::none, Milliseconds(60000));

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();
    auto cmdThread = stdx::thread([&] {
        // Limit dbClient's scope to this thread to test the cancellation callback outliving
        // the caller's client.
        auto dbClient = makeClient();

        auto opKey = UUID::gen();
        auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1), opKey);
        auto fut = dbClient->runCommandRequest(req, baton(), nullptr, cancelSource.token());
        ON_BLOCK_EXIT([&]() { killOp(*fpClient, opKey); });

        pf.promise.setFrom(fut.getNoThrow(interruptible()));
    });

    auto client = getGlobalServiceContext()->getService()->makeClient(__FILE__);
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);

    fpGuard.waitForTimesEntered(opCtx.get(), 1);
    cancelSource.cancel();

    ASSERT_EQ(pf.future.getNoThrow(opCtx.get()), ErrorCodes::CallbackCanceled);
    cmdThread.join();
}

void AsyncClientIntegrationTestFixture::testRunCommandRequestCancelEarly() {
    auto client = makeClient();
    CancellationSource cancelSource;

    cancelSource.cancel();

    auto req = makeTestRequest(DatabaseName::kAdmin,
                               BSON("echo"
                                    << "RunCommandRequestCancelEarly"));
    auto fut = client->runCommandRequest(req, baton(), nullptr, cancelSource.token());
    ASSERT(fut.isReady());
    ASSERT_EQ(fut.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

void AsyncClientIntegrationTestFixture::testRunCommandRequestCancelSourceDismissed() {
    auto client = makeClient();
    auto fpClient = makeClient();
    auto cancelSource = boost::make_optional(CancellationSource());

    auto fpGuard = configureFailCommand(fpClient, "echo", boost::none, Milliseconds(1000));

    auto req = makeTestRequest(DatabaseName::kAdmin,
                               BSON("echo"
                                    << "RunCommandRequestCancelSourceDismissed"));

    auto runCommandFuture = client->runCommandRequest(req, baton(), nullptr, cancelSource->token());
    cancelSource.reset();
    assertOK(runCommandFuture.get(interruptible()));
}

// Tests that cancellation doesn't "miss" if it happens to occur between sending a command
// request and reading its response.
void AsyncClientIntegrationTestFixture::testRunCommandCancelBetweenSendAndRead() {
    auto client = makeClient();
    CancellationSource cancelSource;

    boost::optional<FailPointEnableBlock> fpb("hangBeforeReadResponse");

    auto pf = makePromiseFuture<executor::RemoteCommandResponse>();
    auto commandThread = stdx::thread([&] {
        // Run runCommandRequest in a separate thread in case hanging on the failpoint
        // happens to run on the current thread due to the sending future being immediately
        // available.
        auto req = makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1));
        pf.promise.setFrom(client->runCommandRequest(req, baton(), nullptr, cancelSource.token())
                               .getNoThrow(interruptible()));
    });

    fpb.get()->waitForTimesEntered(fpb->initialTimesEntered() + 1);
    cancelSource.cancel();
    fpb.reset();

    ASSERT_EQ(pf.future.getNoThrow(), ErrorCodes::CallbackCanceled);
    commandThread.join();
}

void AsyncClientIntegrationTestFixture::testExhaustCommand() {
    auto client = makeClient();
    auto opKey = UUID::gen();

    auto helloReq = makeExhaustHello(Milliseconds(100), opKey);
    assertOK(client->beginExhaustCommandRequest(helloReq, baton()).get(interruptible()));

    for (auto i = 0; i < 3; i++) {
        assertOK(client->awaitExhaustCommand(baton()).get(interruptible()));
    }

    auto killOpClient = makeClient();
    killOp(*killOpClient, opKey);

    auto now = getGlobalServiceContext()->getFastClockSource()->now();
    for (auto deadline = now + Seconds(30); now < deadline;
         now = getGlobalServiceContext()->getFastClockSource()->now()) {
        auto finalResp = client->awaitExhaustCommand(baton()).get(interruptible());
        ASSERT_OK(finalResp.status);
        if (getStatusFromCommandResult(finalResp.data).code() == ErrorCodes::Interrupted) {
            break;
        }
    }

    assertOK(
        client->runCommandRequest(makeTestRequest(DatabaseName::kAdmin, BSON("ping" << 1)), baton())
            .get(interruptible()));
}

void AsyncClientIntegrationTestFixture::testBeginExhaustCommandRequestCancel() {
    auto client = makeClient();
    auto fpClient = makeClient();
    CancellationSource cancelSource;

    auto fpGuard = configureFailCommand(fpClient, "hello", boost::none, Milliseconds(60000));

    UUID operationKey = UUID::gen();
    auto helloReq = makeExhaustHello(Milliseconds(100), operationKey);
    auto beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());
    ON_BLOCK_EXIT([&]() { killOp(*fpClient, operationKey); });

    fpGuard.waitForTimesEntered(1);
    cancelSource.cancel();
    ASSERT_EQ(beginExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

void AsyncClientIntegrationTestFixture::testBeginExhaustCommandCancelEarly() {
    auto client = makeClient();
    CancellationSource cancelSource;
    cancelSource.cancel();

    auto beginExhaustFuture = client->beginExhaustCommandRequest(
        makeExhaustHello(Milliseconds(100)), baton(), cancelSource.token());
    ASSERT(beginExhaustFuture.isReady());
    ASSERT_EQ(beginExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

void AsyncClientIntegrationTestFixture::testAwaitExhaustCommandCancel() {
    auto client = makeClient();
    CancellationSource cancelSource;

    // Use long maxAwaitTimeMS.
    auto helloReq = makeExhaustHello(Milliseconds(30000));
    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());

    // The first response should come in quickly.
    assertOK(beginExhaustFuture.get(interruptible()));

    // The second response will take maxAwaitTimeMS.
    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton(), cancelSource.token());
    cancelSource.cancel();
    ASSERT_EQ(awaitExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

void AsyncClientIntegrationTestFixture::testAwaitExhaustCommandCancelEarly() {
    auto client = makeClient();
    CancellationSource cancelSource;

    auto helloReq = makeExhaustHello(Milliseconds(100));
    Future<executor::RemoteCommandResponse> beginExhaustFuture =
        client->beginExhaustCommandRequest(helloReq, baton(), cancelSource.token());
    assertOK(beginExhaustFuture.get(interruptible()));

    cancelSource.cancel();
    Future<executor::RemoteCommandResponse> awaitExhaustFuture =
        client->awaitExhaustCommand(baton(), cancelSource.token());
    ASSERT(awaitExhaustFuture.isReady());
    ASSERT_EQ(awaitExhaustFuture.getNoThrow(interruptible()), ErrorCodes::CallbackCanceled);
}

void AsyncClientIntegrationTestFixture::testEgressNetworkMetrics() {
    auto client = makeClient();
    auto req = makeTestRequest(DatabaseName::kAdmin, BSON("echo" << std::string(1 << 10, 'x')));
    auto const msgLen = static_cast<OpMsgRequest>(req).serialize().size();
    auto caculateResponseMsgSize = [](const BSONObj& responseData) {
        const int msgHeadSize = sizeof(MSGHEADER::Value);
        const int opMsgFlagSize = sizeof(uint32_t);
        const int opMsgSectionFlagSize = sizeof(uint8_t);
        const int msgCheckSumSize = sizeof(uint32_t);
        return msgHeadSize + opMsgFlagSize + opMsgSectionFlagSize + responseData.objsize() +
            msgCheckSumSize;
    };
    auto stats = test::NetworkConnectionStats::get(NetworkCounter::ConnectionType::kEgress);
    auto response = client->runCommandRequest(req, baton()).get(interruptible());
    assertOK(response);
    auto diff = test::NetworkConnectionStats::get(NetworkCounter::ConnectionType::kEgress)
                    .getDifference(stats);
    ASSERT_EQ(diff.logicalBytesOut, msgLen);
    ASSERT_EQ(diff.physicalBytesOut, msgLen + sizeof(uint32_t) /* checksum size */);
    ASSERT_EQ(diff.logicalBytesIn, caculateResponseMsgSize(response.data));
    ASSERT_EQ(diff.physicalBytesIn, caculateResponseMsgSize(response.data));
    ASSERT_EQ(diff.numRequests, 1);
}

}  // namespace mongo::transport
