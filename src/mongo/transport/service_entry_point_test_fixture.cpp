// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/transport/service_entry_point_test_fixture.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_engine_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
#include "mongo/transport/service_entry_point.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/span/span_telemetry_context_impl.h"
#endif

namespace mongo {

using ::testing::Contains;
using ::testing::IsEmpty;

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
    auto swDbResponse = getServiceEntryPoint()
                            ->handleRequest(opCtx, msg, opCtx->fastClockSource().now())
                            .getNoThrow();
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
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(
        cmdBSON, nullptr, Status(ErrorCodes::InternalError, "Test command failure response."));
    logs.stop();
    ASSERT_EQ(logs.countTextContaining(log), 1);
}

void ServiceEntryPointTestFixture::testHandleRequestException(int errorId) {
    const auto handleRequestErrorId = errorId;
    auto opCtx = makeOperationContext();
    const BSONObj dummyObj = BSON("x" << 1);
    const auto dummyNS = "test.foo";
    unittest::LogCaptureGuard logs;
    auto swDbResponse =
        handleRequest(makeUnsupportedOpInsertMessage(dummyNS, &dummyObj, 0), opCtx.get());
    logs.stop();
    ASSERT_NOT_OK(swDbResponse);
    ASSERT_EQ(swDbResponse.getStatus().code(), ErrorCodes::Error(handleRequestErrorId));
    ASSERT_EQ(logs.countTextContaining("Failed to handle request"), 1);
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
    unittest::LogCaptureGuard logs;
    auto swDbResponse = handleRequest(legacyMsg, opCtx.get());
    logs.stop();
    ASSERT_OK(swDbResponse);

    auto legacyReply = rpc::LegacyReply(&swDbResponse.getValue().response);
    auto status = getStatusFromCommandResult(legacyReply.getCommandReply());
    ASSERT_EQ(status.code(), ErrorCodes::UnsupportedOpQueryCommand);

    ASSERT_EQ(logs.countTextContaining(log), 1);
}

void ServiceEntryPointTestFixture::testCommandNotFound(bool logsCommandNotFound) {
    const auto commandName = "testCommandNotFound";
    const auto cmdBSON = BSON(commandName << 1);
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(
        cmdBSON,
        nullptr,
        Status(ErrorCodes::CommandNotFound,
               fmt::format("no such command: '{}'. The client driver may require an upgrade. "
                           "For more details see "
                           "https://dochub.mongodb.org/core/legacy-opcode-removal",
                           commandName)));
    logs.stop();
    if (logsCommandNotFound) {
        ASSERT_EQ(logs.countTextContaining("Command not found in registry"), 1);
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
    auto initialCommandCounter = globalOpCounters().commands->value();
    auto initialQueryCounter = globalOpCounters().queries->value();

    // Test that when commands that return false for `shouldAffect(Query/Command)Counter` are
    // run, the global command and query counters do not increment.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceeds::kCommandName << 1));

        ASSERT_EQ(globalOpCounters().commands->value(), initialCommandCounter);
        ASSERT_EQ(globalOpCounters().queries->value(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectCommandCounter` are run, the global
    // command counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsCommandCounters::kCommandName << 1));

        ASSERT_EQ(globalOpCounters().commands->value(), initialCommandCounter + 1);
        ASSERT_EQ(globalOpCounters().queries->value(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectQueryCounter` are run, the global
    // query counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsQueryCounters::kCommandName << 1));

        ASSERT_EQ(globalOpCounters().commands->value(), initialCommandCounter + 1);
        ASSERT_EQ(globalOpCounters().queries->value(), initialQueryCounter + 1);
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

void ServiceEntryPointTestFixture::testReadConcernClientUnspecifiedNoDefault(
    int expectedApplyDefaultLogCount) {
    unittest::LogCaptureGuard logs;
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1);
    auto opCtx = makeOperationContext();
    runCommandTestWithResponse(cmdBSON, opCtx.get());
    logs.stop();
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(logs.countTextContaining("Applying default readConcern on command"),
              expectedApplyDefaultLogCount);
}

void ServiceEntryPointTestFixture::testReadConcernClientUnspecifiedWithDefault() {
    // When the read concern is not specified, a cluster-wide default read concern applies
    const auto cmdBSON = BSON(TestCmdSucceedsDefaultRCPermitted::kCommandName << 1);
    auto opCtx = makeOperationContext();
    setDefaultReadConcern(opCtx.get(), repl::ReadConcernArgs::kAvailable);
    unittest::LogCaptureGuard logs;
    auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
    logs.stop();
    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());

    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernArgs::kAvailable.getLevel());
    ASSERT_EQ(readConcernArgs.getProvenance(),
              ReadWriteConcernProvenance(ReadWriteConcernProvenanceSourceEnum::customDefault));
    ASSERT_EQ(logs.countTextContaining("Applying default readConcern on command"), 1);
}

void ServiceEntryPointTestFixture::testReadConcernClientSuppliedLevelNotAllowed(
    bool exceptionLogged) {
    // We supply a majority read concern, but the Command object does not support anything
    // non-local, so the ServiceEntryPoint throws.
    const auto cmdBSON =
        BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern" << BSON("level" << "majority"));
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(
        cmdBSON,
        nullptr,
        Status(ErrorCodes::InvalidOptions,
               "Command testSuccess does not support { readConcern: { level: \"majority\", "
               "provenance: \"clientSupplied\" } } :: caused by :: read concern not "
               "supported"));
    logs.stop();
    if (exceptionLogged) {
        ASSERT_EQ(logs.countTextContaining("Assertion while executing command"), 1);
    }
}

void ServiceEntryPointTestFixture::testReadConcernClientSuppliedAllowed() {
    // Supplying a local read concern in the request.
    const auto cmdBSON =
        BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern" << BSON("level" << "local"));
    auto opCtx = makeOperationContext();
    unittest::LogCaptureGuard logs;
    auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
    logs.stop();
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(!readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(logs.countTextContaining("Applying default readConcern on command"), 0);
}

void ServiceEntryPointTestFixture::testReadConcernExtractedOnException() {
    // Even if we throw an exception during the command, read concern is still extracted.
    const auto cmdBSON = BSON(TestCmdFailsRunInvocationWithException::kCommandName
                              << 1 << "readConcern" << BSON("level" << "local"));
    auto opCtx = makeOperationContext();
    unittest::LogCaptureGuard logs;
    auto dbResponse = runCommandTestWithResponse(
        cmdBSON, opCtx.get(), Status(ErrorCodes::InternalError, "Test command failure exception."));
    logs.stop();
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(!readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_EQ(logs.countTextContaining("Applying default readConcern on command"), 0);
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
    const auto wcObj = BSON("w" << "majority"
                                << "wtimeout" << 30);
    const auto cmdBSON =
        BSON(TestCmdSupportsWriteConcern::kCommandName << 1 << "writeConcern" << wcObj);
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(cmdBSON, opCtx.get());
    // ServiceEntryPoint will add the provenance based on the source.
    auto wcObjWithProv = wcObj.addField(BSON("provenance" << "clientSupplied").firstElement());
    auto expectedWC = makeWriteConcernOptions(wcObjWithProv);
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(expectedWC);
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(cmdBSON, opCtx.get());
    logs.stop();
    ASSERT_EQ(logs.countTextContaining("Applying default writeConcern on command"), 0);
}

void ServiceEntryPointTestFixture::testWriteConcernClientUnspecifiedNoDefault() {
    // If write concern is left unspecified in the command, the implicit default write concern is
    // used.
    auto opCtx = makeOperationContext();
    runWriteConcernTestExpectImplicitDefault(opCtx.get());
}

void ServiceEntryPointTestFixture::testWriteConcernClientUnspecifiedWithDefault() {
    // When the write concern is not specified, a cluster-wide default write concern applies
    auto opCtx = makeOperationContext();
    auto defaultWCObj = BSON("w" << "majority"
                                 << "wtimeout" << 500);
    setDefaultWriteConcern(opCtx.get(), defaultWCObj);
    runWriteConcernTestExpectClusterDefault(opCtx.get());
}

void ServiceEntryPointTestFixture::runWriteConcernTestExpectImplicitDefault(
    OperationContext* opCtx) {
    const auto cmdBSON = BSON(TestCmdSupportsWriteConcern::kCommandName << 1);
    auto msg = constructMessage(cmdBSON, opCtx);
    auto expectedWC = makeWriteConcernOptions(BSON("w" << 1 << "wtimeout" << 0 << "provenance"
                                                       << "implicitDefault"));
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(expectedWC);
    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(cmdBSON, opCtx);
    logs.stop();
    ASSERT_EQ(logs.countTextContaining("Applying default writeConcern on command"), 0);
}

void ServiceEntryPointTestFixture::runWriteConcernTestExpectClusterDefault(
    OperationContext* opCtx) {
    const auto cmdBSON = BSON(TestCmdSupportsWriteConcern::kCommandName << 1);
    auto msg = constructMessage(cmdBSON, opCtx);

    // Construct default WC with provenance added.
    auto defaultWCObj =
        ReadWriteConcernDefaults::get(opCtx->getService()).getDefaultWriteConcern(opCtx)->toBSON();
    auto defaultWCObjWithProv =
        defaultWCObj.addField(BSON("provenance" << "customDefault").firstElement());
    auto defaultWCWithProv = makeWriteConcernOptions(defaultWCObjWithProv);
    TestCmdSupportsWriteConcern::setExpectedWriteConcern(defaultWCWithProv);

    unittest::LogCaptureGuard logs;
    runCommandTestWithResponse(cmdBSON, opCtx);
    logs.stop();
    ASSERT_EQ(logs.countTextContaining("Applying default writeConcern"), 1);
}

void ServiceEntryPointTestFixture::testTelemetryContextDeserializedFromSection() {
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        GTEST_SKIP() << "OTel not configured";
    }
    otel::traces::OtelTracesCapturer capturer;
    auto guard = otel::traces::setTraceSamplingFnForTest(
        [](std::string_view, double) { return true; }, [] { return true; });

    auto opCtx = makeOperationContext();

    const static HostAndPort kTestTargetHost = {HostAndPort("FakeHost", 12345)};
    auto request = executor::RemoteCommandRequest{kTestTargetHost,
                                                  DatabaseName::kAdmin,
                                                  BSON(TestCmdSucceeds::kCommandName << 1),
                                                  opCtx.get()};
    auto opMsgRequest = static_cast<OpMsgRequest>(request);
    opMsgRequest.telemetryContext = TelemetryContextSection{
        OtelContextSection{"00-11111111111111111111111111111111-1111111111111111-01"}};
    auto msg = opMsgRequest.serialize();

    auto swDbResponse = handleRequest(msg, opCtx.get());
    EXPECT_EQ(Status::OK(), swDbResponse);

    auto& holder = otel::TelemetryContextHolder::getDecoration(opCtx.get());
    auto retrievedCtx = holder.getTelemetryContext();
    EXPECT_NE(retrievedCtx, nullptr);

    EXPECT_THAT(capturer.getSpans(TestCmdSucceeds::kCommandName), Not(IsEmpty()));
}

void ServiceEntryPointTestFixture::testSpanNotCreatedWhenTelemetryContextNotSetInRequest() {
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        GTEST_SKIP() << "OTel not configured";
    }
    otel::traces::OtelTracesCapturer capturer;
    // We want the sampler to return false so that the only reason the span would be kept is that
    // the telemetry context is set to include a parent span.
    auto samplerGuard = otel::traces::setTraceSamplingFnForTest(
        [](std::string_view, double) { return false; }, [] { return true; });

    auto opCtx = makeOperationContext();
    runCommandTestWithResponse(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());

    EXPECT_THAT(capturer.getSpans(TestCmdSucceeds::kCommandName), IsEmpty());
}

void ServiceEntryPointTestFixture::testIngressSpanHasServerKind() {
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        GTEST_SKIP() << "OTel not configured";
    }
    otel::traces::OtelTracesCapturer capturer;

    auto opCtx = makeOperationContext();
    runCommandTestWithResponse(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());

    EXPECT_THAT(capturer.getSpans(TestCmdSucceeds::kCommandName),
                Contains(otel::traces::HasKind(otel::traces::SpanKind::kServer)));
}

void ServiceEntryPointTestFixture::testIngressSpanHasConsumerKindForMoreToCome() {
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        GTEST_SKIP() << "OTel not configured";
    }
    otel::traces::OtelTracesCapturer capturer;

    auto opCtx = makeOperationContext();
    // A request with the moreToCome flag set is fire-and-forget; the ingress span should be marked
    // as a consumer span rather than a server span.
    auto msg = constructMessage(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());
    OpMsg::setFlag(&msg, OpMsg::kMoreToCome);
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse);

    EXPECT_THAT(capturer.getSpans(TestCmdSucceeds::kCommandName),
                Contains(otel::traces::HasKind(otel::traces::SpanKind::kConsumer)));
}

}  // namespace mongo
