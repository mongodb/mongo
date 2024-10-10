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

#include "mongo/db/service_entry_point_test_fixture.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_INITIALIZER(ServiceEntryPointTestLogSettings)(InitializerContext*) {
    auto& settings = logv2::LogManager::global().getGlobalSettings();
    settings.setMinimumLoggedSeverity(logv2::LogComponent{logv2::LogComponent::kNetwork},
                                      logv2::LogSeverity::Error());
    settings.setMinimumLoggedSeverity(logv2::LogComponent{logv2::LogComponent::kSharding},
                                      logv2::LogSeverity::Error());
    settings.setMinimumLoggedSeverity(logv2::LogComponent{logv2::LogComponent::kCommand},
                                      logv2::LogSeverity::Debug(2));
}

MONGO_REGISTER_COMMAND(TestCmdSucceeds).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithResponse).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithException).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdFailsRunInvocationWithCloseConnectionError).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestHelloCmd).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAdminOnly).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAffectsCommandCounters).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsAffectsQueryCounters).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSucceedsDefaultRCPermitted).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSetsExhaustInvocation).testOnly().forShard();
MONGO_REGISTER_COMMAND(TestCmdSupportsWriteConcern).testOnly().forShard();

class ServiceEntryPointShardRoleTest : public ServiceEntryPointTestFixture {
public:
    void setUp() override {
        ServiceEntryPointTestFixture::setUp();
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("rs0/host1");

        auto replCoordMock = std::make_unique<repl::ReplicationCoordinatorMock>(
            getGlobalServiceContext(), replSettings);
        invariant(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(getGlobalServiceContext(), std::move(replCoordMock));

        getGlobalServiceContext()->getService()->setServiceEntryPoint(
            std::make_unique<ServiceEntryPointShardRole>());
    }
};


TEST_F(ServiceEntryPointShardRoleTest, TestCommandSucceeds) {
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(BSON(TestCmdSucceeds::kCommandName << 1), opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse);
    auto response = dbResponseToBSON(swDbResponse.getValue());
    assertErrorResponseIsExpected(response, Status::OK(), opCtx.get());
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandFailsRunInvocationWithResponse) {
    auto cmdBSON = BSON(TestCmdFailsRunInvocationWithResponse::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSON, nullptr, Status(ErrorCodes::InternalError, "Test command failure response."));
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandFailsRunInvocationWithException) {
    auto cmdBSON = BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSON, nullptr, Status(ErrorCodes::InternalError, "Test command failure response."));
    assertCapturedTextLogsContainSubstr("Assertion while executing command");
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandFailsRunInvocationWithCloseConnectionError) {
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(
        BSON(TestCmdFailsRunInvocationWithCloseConnectionError::kCommandName << 1), opCtx.get());
    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_NOT_OK(swDbResponse);
    ASSERT_EQ(swDbResponse.getStatus().code(), ErrorCodes::SplitHorizonChange);
    assertCapturedTextLogsContainSubstr("Failed to handle request");
}

TEST_F(ServiceEntryPointShardRoleTest, HandleRequestException) {
    const auto handleRequestErrorId = 5745702;
    auto opCtx = makeOperationContext();
    const BSONObj dummyObj = BSON("x" << 1);
    const auto dummyNS = "test.foo";
    auto swDbResponse =
        handleRequest(makeUnsupportedOpInsertMessage(dummyNS, &dummyObj, 0), opCtx.get());
    ASSERT_NOT_OK(swDbResponse);
    ASSERT_EQ(swDbResponse.getStatus().code(), ErrorCodes::Error(handleRequestErrorId));
    assertCapturedTextLogsContainSubstr("Failed to handle request");
}

TEST_F(ServiceEntryPointShardRoleTest, ParseCommandFailsDbQueryUnsupportedCommand) {
    const auto commandName = "testCommandNotSupported";
    const BSONObj obj = BSON(commandName << 1);
    auto dummyLegacyDbQueryBuilder = [&](BufBuilder& b) {
        b.appendNum(0);
        b.appendStr("test.$cmd");
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

    assertCapturedTextLogsContainSubstr("Assertion while parsing command");
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandNotFound) {
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
    assertCapturedTextLogsContainSubstr("Command not found in registry");
}

TEST_F(ServiceEntryPointShardRoleTest, HelloCmdSetsClientMetadata) {
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

TEST_F(ServiceEntryPointShardRoleTest, TestCommentField) {
    auto opCtx = makeOperationContext();
    auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "comment"
                                                      << "Test comment.");
    runCommandTestWithResponse(cmdBSON, opCtx.get());
    auto commentElem = opCtx.get()->getComment();
    ASSERT(commentElem);
    ASSERT_EQ(commentElem->String(), "Test comment.");
}

TEST_F(ServiceEntryPointShardRoleTest, TestHelpField) {
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

TEST_F(ServiceEntryPointShardRoleTest, TestCommandAdminOnlyLog) {
    runCommandTestWithResponse(BSON(TestCmdSucceedsAdminOnly::kCommandName << 1));
    assertCapturedTextLogsContainSubstr("Admin only command");
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandGlobalCounters) {
    auto initialCommandCounter = globalOpCounters.getCommand()->load();
    auto initialQueryCounter = globalOpCounters.getQuery()->load();

    // Test that when commands that return false for `shouldAffect(Query/Command)Counter` are
    // run, the global command and query counters do not increment.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceeds::kCommandName << 1));

        ASSERT_EQ(globalOpCounters.getCommand()->load(), initialCommandCounter);
        ASSERT_EQ(globalOpCounters.getQuery()->load(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectCommandCounter` are run, the global
    // command counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsCommandCounters::kCommandName << 1));

        ASSERT_EQ(globalOpCounters.getCommand()->load(), initialCommandCounter + 1);
        ASSERT_EQ(globalOpCounters.getQuery()->load(), initialQueryCounter);
    }

    // Test that when commands that return true for `shouldAffectQueryCounter` are run, the global
    // query counter increments.
    {
        runCommandTestWithResponse(BSON(TestCmdSucceedsAffectsQueryCounters::kCommandName << 1));

        ASSERT_EQ(globalOpCounters.getCommand()->load(), initialCommandCounter + 1);
        ASSERT_EQ(globalOpCounters.getQuery()->load(), initialQueryCounter + 1);
    }
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandMaxTimeMS) {
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

TEST_F(ServiceEntryPointShardRoleTest, TestCommandMaxTimeMSOpOnly) {
    auto* clkSource = getGlobalServiceContext()->getFastClockSource();
    auto initialMs = clkSource->now();
    // Only internal clients may set maxTimeMSOpOnly.
    getClient()->setIsInternalClient(true);

    // ServiceEntryPoint uses maxTimeMSOpOnly if maxTimeMS is unset.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "maxTimeMSOpOnly"
                                                                << requestMaxTimeMsOpOnly.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        // ServiceEntryPoint should have stored the request's maxTimeMS value, which was previuosly
        // unset.
        opCtx->restoreMaxTimeMS();
        ASSERT_EQ(Date_t::max(), opCtx->getDeadline());
    }

    // ServiceEntryPoint uses maxTimeMs if maxTimeMs is less than maxTimeMsOpOnly.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        const auto requestMaxTimeMs = Milliseconds(100);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName
                                  << 1 << "maxTimeMSOpOnly" << requestMaxTimeMsOpOnly.count()
                                  << "maxTimeMS" << requestMaxTimeMs.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));

        // ServiceEntryPoint did not store anything, the deadline remains the same.
        opCtx->restoreMaxTimeMS();
        // Account for mock clock precision of 1 ms, which is added when using restoreMaxTimeMS.
        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));
    }

    // ServiceEntryPoint uses maxTimeMsOpOnly if maxTimeMs is greater than maxTimeMsOpOnly.
    {
        const auto requestMaxTimeMsOpOnly = Milliseconds(200);
        const auto requestMaxTimeMs = Milliseconds(500);
        auto opCtx = makeOperationContext();
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName
                                  << 1 << "maxTimeMSOpOnly" << requestMaxTimeMsOpOnly.count()
                                  << "maxTimeMS" << requestMaxTimeMs.count());
        runCommandTestWithResponse(cmdBSON, opCtx.get());

        ASSERT_EQ(initialMs + requestMaxTimeMsOpOnly, opCtx->getDeadline());

        // ServiceEntryPoint should have stored the request's maxTimeMS value.
        opCtx->restoreMaxTimeMS();
        // Account for mock clock precision of 1 ms, which is added when using restoreMaxTimeMS.
        ASSERT_LTE(opCtx->getDeadline(), initialMs + requestMaxTimeMs + Milliseconds(1));
    }
}

TEST_F(ServiceEntryPointShardRoleTest, TestOpCtxInterrupt) {
    const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1);
    {
        auto opCtx = makeOperationContext();
        opCtx->markKilled(ErrorCodes::InternalError);
        runCommandTestWithResponse(
            cmdBSON, opCtx.get(), Status(ErrorCodes::InternalError, "operation was interrupted"));
    }

    // ServiceEntryPoint defers handling of `InterruptedDueToReplStateChange` and
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

TEST_F(ServiceEntryPointShardRoleTest, TestReadConcernCommandAllowsLocalNoDefault) {
    // The default Command object does not permit the default readConcern to be applied, and only
    // allows requested readConcern of local to be applied.

    // We don't supply any read concern here, so the implicit default read concern is chosen by
    // the ServiceEntryPoint.
    {
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1);
        auto opCtx = makeOperationContext();
        auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
        ASSERT(readConcernArgs.isImplicitDefault());
        ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
        auto rcLogObj = BSON("readConcernDefault" << BSON("readConcern" << BSON("level"
                                                                                << "local"))
                                                  << "command" << TestCmdSucceeds::kCommandName);
        assertAttrFromCapturedBSON("Applying default readConcern on command", rcLogObj);
    }

    // We supply a majority read concern, but the Command object does not support anything non
    // local, so the ServiceEntryPoint throws.
    {
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern"
                                                                << BSON("level"
                                                                        << "majority"));
        runCommandTestWithResponse(
            cmdBSON,
            nullptr,
            Status(
                ErrorCodes::InvalidOptions,
                "Command testSuccess does not support { readConcern: { level: \"majority\", "
                "provenance: \"clientSupplied\" } } :: caused by :: read concern not supported"));
        assertCapturedTextLogsContainSubstr("Assertion while executing command");
    }

    // Supplying a local read concern in the request.
    {
        const auto cmdBSON = BSON(TestCmdSucceeds::kCommandName << 1 << "readConcern"
                                                                << BSON("level"
                                                                        << "local"));
        auto opCtx = makeOperationContext();
        auto dbResponse = runCommandTestWithResponse(cmdBSON, opCtx.get());
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
        ASSERT(!readConcernArgs.isImplicitDefault());
        ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT_THROWS(
            assertCapturedTextLogsContainSubstr("Applying default readConcern on command"),
            unittest::TestAssertionFailureException);
    }

    // Even if we throw an exception during the command, read concern is still extracted.
    {
        const auto cmdBSON =
            BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1 << "readConcern"
                                                                      << BSON("level"
                                                                              << "local"));
        auto opCtx = makeOperationContext();
        auto dbResponse = runCommandTestWithResponse(
            cmdBSON,
            opCtx.get(),
            Status(ErrorCodes::InternalError, "Test command failure exception."));
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
        ASSERT(!readConcernArgs.isImplicitDefault());
        ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kLocalReadConcern);
        ASSERT_THROWS(
            assertCapturedTextLogsContainSubstr("Applying default readConcern on command"),
            unittest::TestAssertionFailureException);
    }
}

TEST_F(ServiceEntryPointShardRoleTest, TestReadConcernCommandAllowsDefault) {
    // If left unspecified, the default read concern will be used.
    const auto cmdBSON = BSON(TestCmdSucceedsDefaultRCPermitted::kCommandName << 1);
    auto opCtx = makeOperationContext();
    auto msg = constructMessage(cmdBSON, opCtx.get());

    // Set default read concern to majority.
    RWConcernDefault defaults;
    defaults.setDefaultReadConcern(repl::ReadConcernArgs::kMajority);
    ReadWriteConcernDefaults::get(getGlobalServiceContext())
        .setDefault(opCtx.get(), std::move(defaults));

    auto swDbResponse = handleRequest(msg, opCtx.get());
    ASSERT_OK(swDbResponse);

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx.get());
    ASSERT(!readConcernArgs.isImplicitDefault());
    ASSERT_EQ(readConcernArgs.getLevel(), repl::ReadConcernLevel::kMajorityReadConcern);
    auto rcLogObj =
        BSON("readConcernDefault" << BSON("readConcern" << BSON("level"
                                                                << "majority"))
                                  << "command" << TestCmdSucceedsDefaultRCPermitted::kCommandName);
    assertAttrFromCapturedBSON("Applying default readConcern on command", rcLogObj);
}

TEST_F(ServiceEntryPointShardRoleTest, TestCommandInvocationHooks) {
    static int onBeforeRunCounter = 0;
    static int onAfterRunCounter = 0;

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
    };
    CommandInvocationHooks::set(getGlobalServiceContext(),
                                std::make_unique<TestCommandInvocationHooks>());

    ASSERT_EQ(onBeforeRunCounter, 0);
    ASSERT_EQ(onAfterRunCounter, 0);

    const auto cmdBSONSuccess = BSON(TestCmdSucceeds::kCommandName << 1);
    runCommandTestWithResponse(cmdBSONSuccess);
    ASSERT_EQ(onBeforeRunCounter, 1);
    ASSERT_EQ(onAfterRunCounter, 1);

    const auto cmdBSONFailResponse = BSON(TestCmdFailsRunInvocationWithResponse::kCommandName << 1);
    runCommandTestWithResponse(cmdBSONFailResponse,
                               nullptr,
                               Status(ErrorCodes::InternalError, "Test command failure response."));
    ASSERT_EQ(onBeforeRunCounter, 2);
    ASSERT_EQ(onAfterRunCounter, 2);

    const auto cmdBSONFailException =
        BSON(TestCmdFailsRunInvocationWithException::kCommandName << 1);
    runCommandTestWithResponse(
        cmdBSONFailException,
        nullptr,
        Status(ErrorCodes::InternalError, "Test command failure exception."));
    ASSERT_EQ(onBeforeRunCounter, 3);
    ASSERT_EQ(onAfterRunCounter, 2);
}

TEST_F(ServiceEntryPointShardRoleTest, TestExhaustCommandNextInvocationSet) {
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


}  // namespace
}  // namespace mongo
