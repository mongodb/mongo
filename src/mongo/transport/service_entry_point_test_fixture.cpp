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


#include "mongo/transport/service_entry_point_test_fixture.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_engine_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo {

MONGO_REGISTER_COMMAND(TestCmdSucceeds).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithResponse).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithException).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithCloseConnectionError)
    .testOnly()
    .forRouter()
    .forShard();
MONGO_REGISTER_COMMAND(TestHelloCmd).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAdminOnly).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAffectsCommandCounters).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAffectsQueryCounters).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsDefaultRCPermitted).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSetsExhaustInvocation).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestCmdSupportsWriteConcern).testOnly().forRouter().forShard();

WriteConcernOptions TestCmdSupportsWriteConcern::expectedWriteConcern;

void ServiceEntryPointTestFixture::setUp() {
    // Minimal set up necessary for ServiceEntryPoint.
    auto service = getGlobalServiceContext();
    service->setStorageEngine(std::make_unique<StorageEngineMock>());
}

WriteConcernOptions ServiceEntryPointTestFixture::makeWriteConcernOptions(BSONObj wc) {
    auto expectedWCStatusWith = WriteConcernOptions::parse(wc);
    ASSERT(expectedWCStatusWith.isOK());
    return expectedWCStatusWith.getValue();
}

void ServiceEntryPointTestFixture::setDefaultReadConcern(OperationContext* opCtx,
                                                         repl::ReadConcernArgs rc) {
    RWConcernDefault rwcd;
    rwcd.setDefaultReadConcern(repl::ReadConcernArgs::kAvailable);
    ReadWriteConcernDefaults::get(opCtx->getService()).setDefault(opCtx, std::move(rwcd));
}

void ServiceEntryPointTestFixture::setDefaultWriteConcern(OperationContext* opCtx,
                                                          WriteConcernOptions wc) {
    RWConcernDefault rwcd;
    rwcd.setDefaultWriteConcern(wc);
    ReadWriteConcernDefaults::get(opCtx->getService()).setDefault(opCtx, std::move(rwcd));
}

void ServiceEntryPointTestFixture::setDefaultWriteConcern(OperationContext* opCtx, BSONObj obj) {
    setDefaultWriteConcern(opCtx, makeWriteConcernOptions(obj));
}

BSONObj ServiceEntryPointTestFixture::dbResponseToBSON(DbResponse& dbResponse) {
    return OpMsg::parse(dbResponse.response).body;
}

void ServiceEntryPointTestFixture::assertResponseStatus(BSONObj response,
                                                        Status expectedStatus,
                                                        OperationContext* opCtx) {
    auto status = getStatusFromCommandResult(response);
    ASSERT_EQ(CurOp::get(opCtx)->debug().errInfo, expectedStatus);
    ASSERT_EQ(status, expectedStatus);
}

Message ServiceEntryPointTestFixture::constructMessage(BSONObj cmdBSON,
                                                       OperationContext* opCtx,
                                                       uint32_t flags,
                                                       DatabaseName dbName) {
    const static HostAndPort kTestTargetHost = {HostAndPort("FakeHost", 12345)};
    executor::RemoteCommandRequest request{kTestTargetHost, dbName, cmdBSON, opCtx};
    auto msg = static_cast<OpMsgRequest>(request).serialize();
    OpMsg::replaceFlags(&msg, flags);
    return msg;
}

StatusWith<DbResponse> ServiceEntryPointTestFixture::handleRequest(Message msg,
                                                                   OperationContext* opCtx) {
    startCapturingLogMessages();
    auto swDbResponse = getServiceEntryPoint()->handleRequest(opCtx, msg).getNoThrow();
    stopCapturingLogMessages();
    return swDbResponse;
}

DbResponse ServiceEntryPointTestFixture::runCommandTestWithResponse(BSONObj cmdBSON,
                                                                    OperationContext* opCtx,
                                                                    Status expectedStatus) {
    ServiceContext::UniqueOperationContext uniqueOpCtx;
    if (!opCtx) {
        uniqueOpCtx = makeOperationContext();
        opCtx = uniqueOpCtx.get();
    }

    auto msg = constructMessage(cmdBSON, opCtx);
    auto swDbResponse = handleRequest(msg, opCtx);
    ASSERT_OK(swDbResponse);

    auto dbResponse = swDbResponse.getValue();
    auto response = dbResponseToBSON(dbResponse);
    assertResponseStatus(response, expectedStatus, opCtx);
    if (!expectedStatus.isOK()) {
        assertResponseForClusterAndOperationTime(response);
    }

    return swDbResponse.getValue();
}

void ServiceEntryPointTestFixture::assertResponseForClusterAndOperationTime(BSONObj response) {
    ASSERT(response.hasField("$clusterTime")) << ", response: " << response;
    ASSERT(response.hasField("operationTime")) << ", response: " << response;
}

void ServiceEntryPointTestFixture::testCommandSucceeds() {
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse);
    auto response = dbResponseToBSON(swDbResponse.getValue());
    assertResponseStatus(response, Status::OK(), opCtx.get());
}

void ServiceEntryPointTestFixture::testCommandFailsRunInvocationWithResponse() {
    auto cmdBSON = BSON(TestCmdFailsRunInvocationWithResponse::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSON, nullptr, Status(ErrorCodes::InternalError, "Test command failure response."));
}

void ServiceEntryPointTestFixture::testCommandFailsRunInvocationWithException(std::string log) {
    auto cmdBSON = BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSON, nullptr, Status(ErrorCodes::InternalError, "Test command failure response."));
    ASSERT_EQ(countTextFormatLogLinesContaining(log), 1);
}

void ServiceEntryPointTestFixture::testHandleRequestException(int errorId) {
    const auto handleRequestErrorId = errorId;
    auto opCtx = makeOperationContext();
    const BSONObj dummyObj = BSON("x" << 1);
    const auto dummyNS = "test.foo";
    auto swDbResponse =
        handleRequest(makeUnsupportedOpInsertMessage(dummyNS, &dummyObj, 0), opCtx.get());
    ASSERT_NOT_OK(swDbResponse);
    ASSERT_EQ(swDbResponse.getStatus().code(), ErrorCodes::Error(handleRequestErrorId));
    ASSERT_EQ(countTextFormatLogLinesContaining("Failed to handle request"), 1);
}

void ServiceEntryPointTestFixture::testParseCommandFailsDbQueryUnsupportedCommand(std::string log) {
    const auto commandName = "testCommandNotSupported";
    const BSONObj obj = BSON(commandName << 1);
    auto dummyLegacyDbQueryBuilder = [&](BufBuilder& b) {
        b.appendNum(0);
        b.appendCStr("test.$cmd");
        b.appendNum(0);
        // n to return, required to be 1 or -1.
        b.appendNum(1);
        obj.appendSelfToBufBuilder(b);
    };
    // Make dbQuery message with unsupported command.
    auto legacyMsg = makeMessage(dbQuery, dummyLegacyDbQueryBuilder);
    auto opCtx = makeOperationContext();
    auto swDbResponse = handleRequest(legacyMsg, opCtx.get());
    ASSERT_OK(swDbResponse);

    auto legacyReply = rpc::LegacyReply(&swDbResponse.getValue().response);
    auto status = getStatusFromCommandResult(legacyReply.getCommandReply());
    ASSERT_EQ(status.code(), ErrorCodes::UnsupportedOpQueryCommand);

    ASSERT_EQ(countTextFormatLogLinesContaining(log), 1);
}

void ServiceEntryPointTestFixture::testCommandNotFound(bool logsCommandNotFound) {
    const auto commandName = "testCommandNotFound";
    const auto cmdBSON = BSON(commandName << 1);
    runCommandTestWithResponse(
        cmdBSON,
        nullptr,
        Status(ErrorCodes::CommandNotFound,
               fmt::format("no such command: '{}'. The client driver may require an upgrade. "
                           "For more details see "
                           "https://dochub.mongodb.org/core/legacy-opcode-removal",
                           commandName)));
    if (logsCommandNotFound) {
        ASSERT_EQ(countTextFormatLogLinesContaining("Command not found in registry"), 1);
    }
}

void ServiceEntryPointTestFixture::testHelloCmdSetsClientMetadata() {
    // Create a dummy hello command with ClientMetadata.
    HelloCommand hello;
    hello.setDbName(DatabaseName::createDatabaseName_forTest(boost::none, "test"));
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("driverName",
                                               "driverVersion",
                                               "osType",
                                               "osName",
                                               "osArchitecture",
                                               "osVersion",
                                               "appName",
                                               &builder));
    auto obj = builder.obj();
    auto clientMetadata = ClientMetadata::parse(obj["client"]).getValue();
    hello.setClient(clientMetadata);

    // ServiceEntryPoint has not yet set ClientMetadata for the client.
    ASSERT_EQ(ClientMetadata::getForClient(getClient()), nullptr);

    runCommandTestWithResponse(hello.toBSON());

    // Check that ClientMetadata is set.
    BSONObjBuilder bob;
    ClientMetadata::getForClient(getClient())->writeToMetadata(&bob);
    auto clientObj = bob.obj();
    ASSERT(clientObj.hasField("$client"));
}

void ServiceEntryPointTestFixture::testCommentField() {
    auto opCtx = makeOperationContext();
    auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "comment"
                                                      << "Test comment.");
    runCommandTestWithResponse(cmdBSON, opCtx.get());
    auto commentElem = opCtx.get()->getComment();
    ASSERT(commentElem);
    ASSERT_EQ(commentElem->String(), "Test comment.");
}

void ServiceEntryPointTestFixture::testHelpField() {
    const auto cmdBSON =
        BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1 << "help" << true);
    auto dbResponse = runCommandTestWithResponse(cmdBSON);
    auto responseBson = dbResponseToBSON(dbResponse);
    auto helpElem = responseBson.getField("help");
    ASSERT(!helpElem.eoo());
    ASSERT_EQ(helpElem.String(),
              str::stream() << "help for: " << TestCmdFailsRunInvocationWithException::kCommandName
                            << " test command for ServiceEntryPoint");
}

void ServiceEntryPointTestFixture::testCommandServiceCounters(ClusterRole serviceRole) {
    auto opCounters = [&]() -> OpCounters& {
        return serviceOpCounters(serviceRole);
    };
    auto initialCommandCounter = opCounters().getCommand()->load();
    auto initialQueryCounter = opCounters().getQuery()->load();

    // Test that when commands that return false for `shouldAffect(Query/Command)Counter` are
    // run, the global command and query counters do not increment.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceeds::kCommandName << 1));

        ASSERT_EQ(opCounters().getCommand()->load(), initialCommandCounter);
        ASSERT_EQ(opCounters().getQuery()->load(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectCommandCounter` are run, the global
    // command counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsCommandCounters::kCommandName << 1));

        ASSERT_EQ(opCounters().getCommand()->load(), initialCommandCounter + 1);
        ASSERT_EQ(opCounters().getQuery()->load(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectQueryCounter` are run, the global
    // query counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsQueryCounters::kCommandName << 1));

        ASSERT_EQ(opCounters().getCommand()->load(), initialCommandCounter + 1);
        ASSERT_EQ(opCounters().getQuery()->load(), initialQueryCounter + 1);
    }
}

void ServiceEntryPointTestFixture::testCommandMaxTimeMS() {
    auto* clkSource = getGlobalServiceContext()->getFastClockSource();
    auto initialMs = clkSource->now();

    const auto requestMaxTimeMs = Milliseconds(100);
    auto opCtx = makeOperationContext();
    const auto cmdBSON =
        BSON(TestCmdSucceeds::kCommandName << 1 << "maxTimeMS" << requestMaxTimeMs.count());
    runCommandTestWithResponse(cmdBSON, opCtx.get());

    // Deadline should be set to the initial time + requestMaxTimeMs. Also account for mock clock
    // precision of 1 ms, which is added when using restoreMaxTimeMS.
    ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));
}

void ServiceEntryPointTestFixture::testOpCtxInterrupt(bool deferHandling) {
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1);
    {
        auto opCtx = makeOperationContext();
        opCtx->markKilled(ErrorCodes::InternalError);
        runCommandTestWithResponse(
            cmdBSON, opCtx.get(), Status(ErrorCodes::InternalError, "operation was interrupted"));
    }

    if (deferHandling) {
        // ServiceEntryPointShardRole defers handling of `InterruptedDueToReplStateChange` and
        // `PrimarySteppedDown` to the command invocation.
        {
            auto opCtx = makeOperationContext();
            opCtx->markKilled(ErrorCodes::PrimarySteppedDown);
            runCommandTestWithResponse(cmdBSON, opCtx.get());
        }
        {
            auto opCtx = makeOperationContext();
            opCtx->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
            runCommandTestWithResponse(cmdBSON, opCtx.get());
        }
    }
}

void ServiceEntryPointTestFixture::testReadConcernClientUnspecifiedNoDefault() {
    // We don't supply any read concern here, so the implicit default read concern is chosen by
    // the ServiceEntryPoint.
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1);
    auto opCtx = makeOperationContext();
    auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default readConcern on command"), 1);
}

void ServiceEntryPointTestFixture::testReadConcernClientUnspecifiedWithDefault(
    bool expectClusterDefault) {
    // When the read concern is not specified:
    //   * In the router and replica set cases, a cluster-wide default read concern applies
    //   * In the shard server case, an implicit default applies.
    const auto cmdBSON = BSON(TestCmdSucceedsDefaultRCPermitted::kCommandName << 1);
    auto opCtx = makeOperationContext();
    setDefaultReadConcern(opCtx.get(), repl::ReadConcernArgs::kAvailable);
    auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());

    if (expectClusterDefault) {
        ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernArgs::kAvailable.getLevel());
        ASSERT_EQ(readConcernArgs.getProvenance(),
                  ReadWriteConcernProvenance(ReadWriteConcernProvenanceSourceEnum::customDefault));
        ASSERT_EQ(countTextFormatLogLinesContaining("Applying default readConcern on command"), 1);
    } else {
        ASSERT(readConcernArgs.isImplicitDefault());
        ASSERT_EQ(countTextFormatLogLinesContaining("Applying default readConcern on command"), 0);
    }
}

void ServiceEntryPointTestFixture::testReadConcernClientSuppliedLevelNotAllowed(
    bool exceptionLogged) {
    // We supply a majority read concern, but the Command object does not support anything
    // non-local, so the ServiceEntryPoint throws.
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern"
                                                            << BSON("level"
                                                                    << "majority"));
    runCommandTestWithResponse(
        cmdBSON,
        nullptr,
        Status(ErrorCodes::InvalidOptions,
               "Command testSuccess does not support { readConcern: { level: \"majority\", "
               "provenance: \"clientSupplied\" } } :: caused by :: read concern not "
               "supported"));
    if (exceptionLogged) {
        ASSERT_EQ(countTextFormatLogLinesContaining("Assertion while executing command"), 1);
    }
}

void ServiceEntryPointTestFixture::testReadConcernClientSuppliedAllowed() {
    // Supplying a local read concern in the request.
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern"
                                                            << BSON("level"
                                                                    << "local"));
    auto opCtx = makeOperationContext();
    auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(!readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default readConcern on command"), 0);
}

void ServiceEntryPointTestFixture::testReadConcernExtractedOnException() {
    // Even if we throw an exception during the command, read concern is still extracted.
    const auto cmdBSON =
        BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1 << "readConcern"
                                                                  << BSON("level"
                                                                          << "local"));
    auto opCtx = makeOperationContext();
    auto dbResponse = runCommandTestWithResponse(
        cmdBSON, opCtx.get(), Status(ErrorCodes::InternalError, "Test command failure exception."));
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(!readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default readConcern on command"), 0);
}

void ServiceEntryPointTestFixture::testCommandInvocationHooks() {

    class TestCommandInvocationHooks final : public CommandInvocationHooks {
    public:
        void onBeforeRun(OperationContext*, CommandInvocation*) override {
            ++onBeforeRunCounter;
        }

        void onAfterRun(OperationContext*,
                        CommandInvocation*,
                        rpc::ReplyBuilderInterface*) override {
            ++onAfterRunCounter;
        }
        int onBeforeRunCounter = 0;
        int onAfterRunCounter = 0;
    };

    auto hooks = std::make_unique<TestCommandInvocationHooks>();
    auto hooksPtr = hooks.get();
    CommandInvocationHooks::set(getGlobalServiceContext(), std::move(hooks));

    ASSERT_EQ(hooksPtr->onBeforeRunCounter, 0);
    ASSERT_EQ(hooksPtr->onAfterRunCounter, 0);

    const auto cmdBSONSuccess = BSON(TestCmdSucceeds::kCommandName << 1);
    runCommandTestWithResponse(cmdBSONSuccess);
    ASSERT_EQ(hooksPtr->onBeforeRunCounter, 1);
    ASSERT_EQ(hooksPtr->onAfterRunCounter, 1);

    const auto cmdBSONFailResponse = BSON(TestCmdFailsRunInvocationWithResponse::kCommandName << 1);
    runCommandTestWithResponse(cmdBSONFailResponse,
                               nullptr,
                               Status(ErrorCodes::InternalError, "Test command failure response."));
    ASSERT_EQ(hooksPtr->onBeforeRunCounter, 2);
    ASSERT_EQ(hooksPtr->onAfterRunCounter, 2);

    const auto cmdBSONFailException =
        BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSONFailException,
        nullptr,
        Status(ErrorCodes::InternalError, "Test command failure exception."));
    ASSERT_EQ(hooksPtr->onBeforeRunCounter, 3);
    ASSERT_EQ(hooksPtr->onAfterRunCounter, 2);
}

void ServiceEntryPointTestFixture::testExhaustCommandNextInvocationSet() {
    const auto cmdBSONSuccess = BSON(TestCmdSetsExhaustInvocation::kCommandName << 1);
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(cmdBSONSuccess, opCtx.get(), OpMsg::kExhaustSupported);
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse.getStatus());
    auto dbResponse = swDbResponse.getValue();
    ASSERT(opCtx->isExhaust());
    ASSERT(dbResponse.shouldRunAgainForExhaust);
    ASSERT(dbResponse.nextInvocation);
    ASSERT_EQ(TestCmdSetsExhaustInvocation::kCommandName,
              dbResponse.nextInvocation->firstElementFieldNameStringData());
}

void ServiceEntryPointTestFixture::testWriteConcernClientSpecified() {
    // Test client supplied write concerns.
    const auto wcObj = BSON("w"
                            << "majority"
                            << "wtimeout" << 30);
    const auto cmdBSON =
        BSON(TestCmdSupportsWriteConcern::kCommandName << 1 << "writeConcern" << wcObj);
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(cmdBSON, opCtx.get());
    // ServiceEntryPoint will add the provenance based on the source.
    auto wcObjWithProv = wcObj.addField(BSON("provenance"
                                             << "clientSupplied")
                                            .firstElement());
    auto expectedWC = makeWriteConcernOptions(wcObjWithProv);
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(expectedWC);
    runCommandTestWithResponse(cmdBSON, opCtx.get());
    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default writeConcern on command"), 0);
}

void ServiceEntryPointTestFixture::testWriteConcernClientUnspecifiedNoDefault() {
    // If write concern is left unspecified in the command, the implicit default write concern is
    // used.
    auto opCtx = makeOperationContext();
    runWriteConcernTestExpectImplicitDefault(opCtx.get());
}

void ServiceEntryPointTestFixture::testWriteConcernClientUnspecifiedWithDefault(
    bool expectClusterDefault) {
    // When the write concern is not specified:
    //   * In the router and replica set cases, a cluster-wide default write concern applies
    //   * In the shard server case, an implicit default applies.
    auto opCtx = makeOperationContext();
    auto defaultWCObj = BSON("w"
                             << "majority"
                             << "wtimeout" << 500);
    setDefaultWriteConcern(opCtx.get(), defaultWCObj);
    if (expectClusterDefault) {
        runWriteConcernTestExpectClusterDefault(opCtx.get());
    } else {
        runWriteConcernTestExpectImplicitDefault(opCtx.get());
    }
}

void ServiceEntryPointTestFixture::runWriteConcernTestExpectImplicitDefault(
    OperationContext* opCtx) {
    const auto cmdBSON = BSON(TestCmdSupportsWriteConcern::kCommandName << 1);
    auto msg = constructMessage(cmdBSON, opCtx);
    auto expectedWC = makeWriteConcernOptions(BSON("w" << 1 << "wtimeout" << 0 << "provenance"
                                                       << "implicitDefault"));
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(expectedWC);
    runCommandTestWithResponse(cmdBSON, opCtx);
    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default writeConcern on command"), 0);
}

void ServiceEntryPointTestFixture::runWriteConcernTestExpectClusterDefault(
    OperationContext* opCtx) {
    const auto cmdBSON = BSON(TestCmdSupportsWriteConcern::kCommandName << 1);
    auto msg = constructMessage(cmdBSON, opCtx);

    // Construct default WC with provenance added.
    auto defaultWCObj =
        ReadWriteConcernDefaults::get(opCtx->getService()).getDefaultWriteConcern(opCtx)->toBSON();
    auto defaultWCObjWithProv = defaultWCObj.addField(BSON("provenance"
                                                           << "customDefault")
                                                          .firstElement());
    auto defaultWCWithProv = makeWriteConcernOptions(defaultWCObjWithProv);
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(defaultWCWithProv);

    runCommandTestWithResponse(cmdBSON, opCtx);

    ASSERT_EQ(countTextFormatLogLinesContaining("Applying default writeConcern"), 1);
}

}  // namespace mongo
