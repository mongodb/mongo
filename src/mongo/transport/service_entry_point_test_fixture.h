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

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

class ServiceEntryPointTestFixture : public ServiceContextTest {
public:
    ServiceEntryPointTestFixture()
        : ServiceContextTest(std::make_unique<ScopedGlobalServiceContextForTest>(
                                 ServiceContext::make(std::make_unique<ClockSourceMock>(),
                                                      std::make_unique<ClockSourceMock>(),
                                                      std::make_unique<TickSourceMock<>>()),
                                 shouldSetupTL),
                             std::make_shared<transport::MockSession>(nullptr)) {}

    void setUp() override;

    ServiceContext::UniqueOperationContext makeOperationContext() {
        return getClient()->makeOperationContext();
    }

    BSONObj dbResponseToBSON(DbResponse& dbResponse);

    virtual void assertResponseStatus(BSONObj response,
                                      Status expectedStatus,
                                      OperationContext* opCtx);

    Message constructMessage(BSONObj cmdBSON,
                             OperationContext* opCtx,
                             uint32_t flags = 0,
                             DatabaseName dbName = DatabaseName::kAdmin);

    StatusWith<DbResponse> handleRequest(Message msg, OperationContext* opCtx = nullptr);

    DbResponse runCommandTestWithResponse(BSONObj cmdBSON,
                                          OperationContext* opCtx = nullptr,
                                          Status expectedStatus = Status::OK());

    virtual void assertResponseForClusterAndOperationTime(BSONObj response);

    ServiceEntryPoint* getServiceEntryPoint() {
        return getClient()->getService()->getServiceEntryPoint();
    }

    void testCommandSucceeds();
    void testCommandFailsRunInvocationWithResponse();
    void testCommandFailsRunInvocationWithException(std::string log);
    void testHandleRequestException(int errorId);
    void testParseCommandFailsDbQueryUnsupportedCommand(std::string log);
    void testCommandNotFound(bool logsCommandNotFound);
    void testHelloCmdSetsClientMetadata();
    void testCommentField();
    void testHelpField();
    void testCommandServiceCounters(ClusterRole serviceRole);
    void testCommandMaxTimeMS();
    void testOpCtxInterrupt(bool deferHandling);
    void testReadConcernClientUnspecifiedNoDefault();
    void testReadConcernClientUnspecifiedWithDefault(bool expectClusterDefault);
    void testReadConcernClientSuppliedLevelNotAllowed(bool exceptionLogged);
    void testReadConcernClientSuppliedAllowed();
    void testReadConcernExtractedOnException();
    void testCommandInvocationHooks();
    void testExhaustCommandNextInvocationSet();
    void testWriteConcernClientSpecified();
    void testWriteConcernClientUnspecifiedNoDefault();
    void testWriteConcernClientUnspecifiedWithDefault(bool expectClusterDefault);

protected:
    ReadWriteConcernDefaultsLookupMock _lookupMock;

    void runWriteConcernTestExpectImplicitDefault(OperationContext* opCtx);
    void runWriteConcernTestExpectClusterDefault(OperationContext* opCtx);
    WriteConcernOptions makeWriteConcernOptions(BSONObj wc);
    void setDefaultReadConcern(OperationContext* opCtx, repl::ReadConcernArgs rc);
    void setDefaultWriteConcern(OperationContext* opCtx, WriteConcernOptions wc);
    void setDefaultWriteConcern(OperationContext* opCtx, BSONObj obj);

private:
    unittest::MinimumLoggedSeverityGuard logSeverityGuardNetwork{logv2::LogComponent::kNetwork,
                                                                 logv2::LogSeverity::Error()};
    unittest::MinimumLoggedSeverityGuard logSeverityGuardReplication{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(2)};
    unittest::MinimumLoggedSeverityGuard logSeverityGuardSharding{logv2::LogComponent::kSharding,
                                                                  logv2::LogSeverity::Debug(2)};
    unittest::MinimumLoggedSeverityGuard logSeverityGuardCommand{logv2::LogComponent::kCommand,
                                                                 logv2::LogSeverity::Debug(2)};
};

class TestCmdBase : public BasicCommand {
public:
    TestCmdBase(std::string cmdName) : BasicCommand(cmdName) {}
    std::string help() const override {
        return "test command for ServiceEntryPoint";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool shouldAffectCommandCounter() const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuth() const final {
        return false;
    }

    virtual bool runWithBuilderOnly(BSONObjBuilder& result) = 0;

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        return runWithBuilderOnly(result);
    }
};

class TestCmdSucceeds : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testSuccess";
    TestCmdSucceeds() : TestCmdBase(kCommandName) {}
    TestCmdSucceeds(std::string cmdName) : TestCmdBase(cmdName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        return true;
    }
};

class TestCmdProcessInternalSucceedCommand : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testCmdProcessInternalSucceedCommand";
    TestCmdProcessInternalSucceedCommand() : TestCmdBase(kCommandName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        return true;
    }
};

class TestCmdProcessInternalCommand : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testProcessInternalCommand";
    TestCmdProcessInternalCommand() : TestCmdBase(kCommandName) {}

    bool isSubjectToIngressAdmissionControl() const override {
        return true;
    }

    // We want to reuse all the members from the base class except this shortcut.
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        uassert(ErrorCodes::InternalError, "runWithBuilderOnly not implemented", false);
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        // Run the list command as subcommand.
        auto cmdBSON = BSON(TestCmdProcessInternalSucceedCommand::kCommandName << 1);

        constexpr auto dummyDatabaseName = StringData{"dummy_database_name"};
        BSONObj info;
        DBDirectClient{opCtx}.runCommand(
            DatabaseName{dummyDatabaseName.data(), dummyDatabaseName.size()}, cmdBSON, info);
        return true;
    }
};

class TestCmdFailsRunInvocationWithResponse : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testFailResponse";
    TestCmdFailsRunInvocationWithResponse() : TestCmdBase(kCommandName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        CommandHelpers::appendCommandStatusNoThrow(
            result, Status(ErrorCodes::InternalError, "Test command failure response."));
        return false;
    }
};

class TestCmdFailsRunInvocationWithException : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testFailException";
    TestCmdFailsRunInvocationWithException() : TestCmdBase(kCommandName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        uasserted(ErrorCodes::InternalError, "Test command failure exception.");
        return true;
    }
};

class TestCmdFailsRunInvocationWithCloseConnectionError : public TestCmdBase {
public:
    static constexpr auto kCommandName = "testFailCloseConection";
    TestCmdFailsRunInvocationWithCloseConnectionError() : TestCmdBase(kCommandName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        uasserted(ErrorCodes::SplitHorizonChange, "Test command failure close connection.");
        return true;
    }
};

// Mimics hello command, because the actual implementation relies on real network configuration.
class TestHelloCmd : public TestCmdBase {
public:
    static constexpr auto kCommandName = "hello";
    TestHelloCmd() : TestCmdBase(kCommandName) {}
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        return true;
    }
};

class TestCmdSucceedsAdminOnly : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessAdmin";
    TestCmdSucceedsAdminOnly() : TestCmdSucceeds(kCommandName) {}
    bool adminOnly() const override {
        return true;
    }
};

class TestCmdSucceedsAffectsCommandCounters : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessCommandCounter";
    TestCmdSucceedsAffectsCommandCounters() : TestCmdSucceeds(kCommandName) {}
    bool shouldAffectCommandCounter() const override {
        return true;
    }
};

class TestCmdSucceedsAffectsQueryCounters : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessQueryCounter";
    TestCmdSucceedsAffectsQueryCounters() : TestCmdSucceeds(kCommandName) {}
    bool shouldAffectQueryCounter() const override {
        return true;
    }
};

class TestCmdSucceedsDefaultRCPermitted : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessDefaultRC";
    TestCmdSucceedsDefaultRCPermitted() : TestCmdSucceeds(kCommandName) {}
    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return {{Status::OK()}, {Status::OK()}};
    }
    bool runWithBuilderOnly(BSONObjBuilder& result) override {
        return true;
    }
};

class TestCmdSetsExhaustInvocation : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessExhaust";
    TestCmdSetsExhaustInvocation() : TestCmdSucceeds(kCommandName) {}
    bool runWithReplyBuilder(OperationContext*,
                             const DatabaseName&,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) override {
        replyBuilder->setNextInvocation(cmdObj);
        return true;
    }
};

class TestCmdSupportsWriteConcern : public TestCmdSucceeds {
public:
    static constexpr auto kCommandName = "testSuccessWriteConcern";
    static WriteConcernOptions expectedWriteConcern;
    static void setExpectedWriteConcern(WriteConcernOptions wc) {
        TestCmdSupportsWriteConcern::expectedWriteConcern = wc;
    }
    static WriteConcernOptions getExpectedWriteConcern() {
        return TestCmdSupportsWriteConcern::expectedWriteConcern;
    }
    TestCmdSupportsWriteConcern() : TestCmdSucceeds(kCommandName) {}
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }
    bool runWithReplyBuilder(OperationContext* opCtx,
                             const DatabaseName&,
                             const BSONObj&,
                             rpc::ReplyBuilderInterface*) override {
        if (getExpectedWriteConcern() != opCtx->getWriteConcern()) {
            uassertStatusOK(Status(
                ErrorCodes::InternalError,
                fmt::format(
                    "Expected write concern {}, got {}",
                    TestCmdSupportsWriteConcern::getExpectedWriteConcern().toBSON().toString(),
                    opCtx->getWriteConcern().toBSON().toString())));
        }
        return true;
    }
};

}  // namespace mongo
