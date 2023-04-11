/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands_test_example_gen.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

TEST(Commands, appendCommandStatusOK) {
    BSONObjBuilder actualResult;
    CommandHelpers::appendCommandStatusNoThrow(actualResult, Status::OK());

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 1.0);

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusError) {
    BSONObjBuilder actualResult;
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusNoOverwrite) {
    BSONObjBuilder actualResult;
    actualResult.append("a", "b");
    actualResult.append("c", "d");
    actualResult.append("ok", 0.0);
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("a", "b");
    expectedResult.append("c", "d");
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

TEST(Commands, appendCommandStatusErrorExtraInfo) {
    BSONObjBuilder actualResult;
    const Status status(ErrorExtraInfoExample(123), "not again!");
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);

    BSONObjBuilder expectedResult;
    expectedResult.append("ok", 0.0);
    expectedResult.append("errmsg", status.reason());
    expectedResult.append("code", status.code());
    expectedResult.append("codeName", ErrorCodes::errorString(status.code()));
    expectedResult.append("data", 123);

    ASSERT_BSONOBJ_EQ(actualResult.obj(), expectedResult.obj());
}

DEATH_TEST(Commands, appendCommandStatusInvalidOkValue, "invariant") {
    BSONObjBuilder actualResult;
    actualResult.append("a", "b");
    actualResult.append("c", "d");
    actualResult.append("ok", "yes");
    const Status status(ErrorCodes::InvalidLength, "fake error for test");

    // An "ok" value other than 1.0 or 0.0 is not allowed and should crash.
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);
}

DEATH_TEST(Commands, appendCommandStatusNoCodeName, "invariant") {
    BSONObjBuilder actualResult;
    actualResult.append("a", "b");
    actualResult.append("code", ErrorCodes::InvalidLength);
    actualResult.append("ok", 1.0);
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");

    // If the result already has an error code, we don't move any code or codeName over from the
    // status. Therefore, if the result has an error code but no codeName, we're missing a
    // required field and should crash.
    CommandHelpers::appendCommandStatusNoThrow(actualResult, status);
}

class ParseNsOrUUID : public ServiceContextTest {
public:
    ParseNsOrUUID() : opCtxPtr(makeOperationContext()), opCtx(opCtxPtr.get()) {}
    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;
};

TEST_F(ParseNsOrUUID, FailWrongType) {
    auto cmd = BSON("query" << BSON("a" << BSON("$gte" << 11)));
    ASSERT_THROWS_CODE(CommandHelpers::parseNsOrUUID(
                           DatabaseName::createDatabaseName_forTest(boost::none, "db"), cmd),
                       DBException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailEmptyDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(CommandHelpers::parseNsOrUUID(DatabaseName(), cmd),
                       DBException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, FailInvalidDbName) {
    auto cmd = BSON("query"
                    << "coll");
    ASSERT_THROWS_CODE(CommandHelpers::parseNsOrUUID(
                           DatabaseName::createDatabaseName_forTest(boost::none, "test.coll"), cmd),
                       DBException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, ParseValidColl) {
    auto cmd = BSON("query"
                    << "coll");
    auto parsedNss = CommandHelpers::parseNsOrUUID(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"), cmd);
    ASSERT_EQ(*parsedNss.nss(), NamespaceString::createNamespaceString_forTest("test.coll"));
}

TEST_F(ParseNsOrUUID, ParseValidUUID) {
    const UUID uuid = UUID::gen();
    auto cmd = BSON("query" << uuid);
    auto parsedNsOrUUID = CommandHelpers::parseNsOrUUID(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"), cmd);
    ASSERT_EQUALS(uuid, *parsedNsOrUUID.uuid());
}

/**
 * TypedCommand test
 */
class ExampleIncrementCommand final : public TypedCommand<ExampleIncrementCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = commands_test_example::ExampleIncrement;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        auto typedRun(OperationContext* opCtx) {
            commands_test_example::ExampleIncrementReply r;
            r.setIPlusOne(request().getI() + 1);
            return r;
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};

// Just like ExampleIncrementCommand, but using the MinimalInvocationBase.
class ExampleMinimalCommand final : public TypedCommand<ExampleMinimalCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Return an incremented request.i. Example of a simple TypedCommand.";
    }

public:
    using Request = commands_test_example::ExampleMinimal;

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

        /**
         * Reply with an incremented 'request.i'.
         */
        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            commands_test_example::ExampleIncrementReply r;
            r.setIPlusOne(request().getI() + 1);
            reply->fillFrom(r);
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {}

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };
};

// Just like ExampleIncrementCommand, but with a void typedRun.
class ExampleVoidCommand final : public TypedCommand<ExampleVoidCommand> {
private:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }

public:
    using Request = commands_test_example::ExampleVoid;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        /**
         * Have some testable side-effect.
         */
        void typedRun(OperationContext*) {
            static_cast<const ExampleVoidCommand*>(definition())->iCapture = request().getI() + 1;
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {}

        void doCheckAuthorization(OperationContext*) const override {}

        /**
         * The ns() for when Request's IDL specifies "namespace: concatenate_with_db".
         */
        NamespaceString ns() const override {
            return request().getNamespace();
        }
    };

    mutable std::int32_t iCapture = 0;
};

template <typename Fn, typename AuthFn>
class MyCommand final : public TypedCommand<MyCommand<Fn, AuthFn>> {
public:
    class Invocation final : public TypedCommand<MyCommand>::InvocationBase {
    public:
        using Base = typename TypedCommand<MyCommand>::InvocationBase;
        using Base::Base;

        auto typedRun(OperationContext*) const {
            return _command()->_fn();
        }

    private:
        NamespaceString ns() const override {
            return Base::request().getNamespace();
        }
        bool supportsWriteConcern() const override {
            return false;
        }
        void doCheckAuthorization(OperationContext* opCtx) const override {
            return _command()->_authFn();
        }

        const MyCommand* _command() const {
            return static_cast<const MyCommand*>(Base::definition());
        }
    };

    using Request = commands_test_example::ExampleVoid;

    MyCommand(StringData name, Fn fn, AuthFn authFn)
        : TypedCommand<MyCommand<Fn, AuthFn>>(name),
          _fn{std::move(fn)},
          _authFn{std::move(authFn)} {}

private:
    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Accepts Request and returns void.";
    }

    Fn _fn;
    AuthFn _authFn;
};

template <typename Fn, typename AuthFn>
using CmdT = MyCommand<typename std::decay<Fn>::type, typename std::decay<AuthFn>::type>;

auto throwFn = [] {
    uasserted(ErrorCodes::UnknownError, "some error");
};
auto authSuccessFn = [] {
    return;
};
auto authFailFn = [] {
    uasserted(ErrorCodes::Unauthorized, "Not authorized");
};

ExampleIncrementCommand exampleIncrementCommand;
ExampleMinimalCommand exampleMinimalCommand;
ExampleVoidCommand exampleVoidCommand;
CmdT<decltype(throwFn), decltype(authSuccessFn)> throwStatusCommand("throwsStatus",
                                                                    throwFn,
                                                                    authSuccessFn);
CmdT<decltype(throwFn), decltype(authFailFn)> unauthorizedCommand("unauthorizedCmd",
                                                                  throwFn,
                                                                  authFailFn);

class TypedCommandTest : public ServiceContextMongoDTest {
public:
    void setUp() {
        ServiceContextMongoDTest::setUp();

        // Set up the auth subsystem to authorize the command.
        auto localManagerState = std::make_unique<AuthzManagerExternalStateMock>();
        _managerState = localManagerState.get();
        _managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        auto uniqueAuthzManager = std::make_unique<AuthorizationManagerImpl>(
            getServiceContext(), std::move(localManagerState));
        _authzManager = uniqueAuthzManager.get();
        AuthorizationManager::set(getServiceContext(), std::move(uniqueAuthzManager));
        _authzManager->setAuthEnabled(true);

        _session = _transportLayer.createSession();
        _client = getServiceContext()->makeClient("testClient", _session);
        RestrictionEnvironment::set(
            _session, std::make_unique<RestrictionEnvironment>(SockAddr(), SockAddr()));
        _authzSession = AuthorizationSession::get(_client.get());

        // Insert a user document that will represent the user used for running the commands.
        auto credentials =
            BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                      "a", saslGlobalParams.scramSHA1IterationCount.load())
                               << "SCRAM-SHA-256"
                               << scram::Secrets<SHA256Block>::generateCredentials(
                                      "a", saslGlobalParams.scramSHA256IterationCount.load()));

        BSONObj userDoc = BSON("_id"_sd
                               << "test.varun"_sd
                               << "user"_sd
                               << "varun"
                               << "db"_sd
                               << "test"
                               << "credentials"_sd << credentials << "roles"_sd
                               << BSON_ARRAY(BSON("role"_sd
                                                  << "readWrite"_sd
                                                  << "db"_sd
                                                  << "test"_sd)));

        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_managerState->insertPrivilegeDocument(opCtx.get(), userDoc, {}));
    }

protected:
    TypedCommandTest() : ServiceContextMongoDTest(Options{}.useMockClock(true)) {}

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    template <typename T>
    void runIncr(T& command, std::function<void(int, const BSONObj&)> postAssert) {
        const NamespaceString ns = NamespaceString::createNamespaceString_forTest("testdb.coll");

        for (std::int32_t i : {123, 12345, 0, -456}) {
            const OpMsgRequest request = [&] {
                typename T::Request incr(ns);
                incr.setI(i);
                return incr.serialize(BSON("$db" << ns.db()));
            }();

            auto opCtx = _client->makeOperationContext();
            auto invocation = command.parse(opCtx.get(), request);

            ASSERT_EQ(invocation->ns(), ns);

            const BSONObj reply = [&] {
                rpc::OpMsgReplyBuilder replyBuilder;
                try {
                    invocation->checkAuthorization(opCtx.get(), request);
                    invocation->run(opCtx.get(), &replyBuilder);
                    auto bob = replyBuilder.getBodyBuilder();
                    CommandHelpers::extractOrAppendOk(bob);
                } catch (const DBException& e) {
                    auto bob = replyBuilder.getBodyBuilder();
                    CommandHelpers::appendCommandStatusNoThrow(bob, e.toStatus());
                }
                return replyBuilder.releaseBody();
            }();

            postAssert(i, reply);
        }
    }

protected:
    AuthorizationManager* _authzManager;
    AuthzManagerExternalStateMock* _managerState;
    transport::TransportLayerMock _transportLayer;
    std::shared_ptr<transport::Session> _session;
    ServiceContext::UniqueClient _client;
    AuthorizationSession* _authzSession;
};

const UserName kVarunTest("varun", "test");
const UserRequest kVarunTestRequest(kVarunTest, boost::none);

TEST_F(TypedCommandTest, runTyped) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleIncrementCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(reply["iPlusOne"].Int(), i + 1);
    });
}

TEST_F(TypedCommandTest, runMinimal) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleMinimalCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(reply["iPlusOne"].Int(), i + 1);
    });
}

TEST_F(TypedCommandTest, runVoid) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleVoidCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(exampleVoidCommand.iCapture, i + 1);
    });
}

TEST_F(TypedCommandTest, runThrowStatus) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(throwStatusCommand, [](int i, const BSONObj& reply) {
        Status status = Status::OK();
        try {
            (void)throwFn();
        } catch (const DBException& e) {
            status = e.toStatus();
        }
        ASSERT_EQ(reply["ok"].Double(), 0.0);
        ASSERT_EQ(reply["errmsg"].String(), status.reason());
        ASSERT_EQ(reply["code"].Int(), status.code());
        ASSERT_EQ(reply["codeName"].String(), ErrorCodes::errorString(status.code()));
    });
}

TEST_F(TypedCommandTest, runThrowDoCheckAuthorization) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(unauthorizedCommand, [](int i, const BSONObj& reply) {
        Status status = Status::OK();
        try {
            (void)authFailFn();
        } catch (const DBException& e) {
            status = e.toStatus();
        }
        ASSERT_EQ(reply["ok"].Double(), 0.0);
        ASSERT_EQ(reply["code"].Int(), status.code());
        ASSERT_EQ(reply["codeName"].String(), ErrorCodes::errorString(status.code()));
    });
}

TEST_F(TypedCommandTest, runThrowNoUserAuthenticated) {
    {
        // Don't authenticate any users.
        auto opCtx = _client->makeOperationContext();
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleIncrementCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 0.0);
        ASSERT_EQ(reply["errmsg"].String(),
                  str::stream() << "Command exampleIncrement requires authentication");
        ASSERT_EQ(reply["code"].Int(), ErrorCodes::Unauthorized);
        ASSERT_EQ(reply["codeName"].String(), ErrorCodes::errorString(ErrorCodes::Unauthorized));
    });
}

TEST_F(TypedCommandTest, runThrowAuthzSessionExpired) {
    {
        // Load user into the authorization session and then expire it.
        auto opCtx = _client->makeOperationContext();
        auto expirationTime = clockSource()->now() + Hours{1};
        ASSERT_OK(
            _authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, expirationTime));

        // Fast-forward time before starting a new request.
        clockSource()->advance(Hours(2));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleIncrementCommand, [](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 0.0);
        ASSERT_EQ(
            reply["errmsg"].String(),
            str::stream() << "Command exampleIncrement requires reauthentication since the current "
                             "authorization session has expired. Please re-auth.");
        ASSERT_EQ(reply["code"].Int(), ErrorCodes::ReauthenticationRequired);
        ASSERT_EQ(reply["codeName"].String(),
                  ErrorCodes::errorString(ErrorCodes::ReauthenticationRequired));
    });
}

}  // namespace
}  // namespace mongo
