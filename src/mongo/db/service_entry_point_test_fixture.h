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


#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point.h"
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

    void assertErrorResponseIsExpected(BSONObj response,
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

    void assertResponseForClusterAndOperationTime(BSONObj response);

    void assertCapturedTextLogsContainSubstr(std::string substr);

    // Check `attr` from the log that matches with `msg`.
    void assertAttrFromCapturedBSON(std::string msg, BSONObj check);

    ServiceEntryPoint* getServiceEntryPoint() {
        return getClient()->getService()->getServiceEntryPoint();
    }

private:
    ReadWriteConcernDefaultsLookupMock _lookupMock;
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
    TestCmdSupportsWriteConcern() : TestCmdSucceeds(kCommandName) {}
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
};

}  // namespace mongo
