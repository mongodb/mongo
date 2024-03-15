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

#include <boost/none.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <initializer_list>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands_test_example.h"
#include "mongo/db/commands_test_example_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using namespace fmt::literals;

using service_context_test::RoleOverride;
using service_context_test::ServerRoleIndex;

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

TEST_F(ParseNsOrUUID, FailInvalidCollectionNameContainsDollar) {
    auto cmd = BSON("query"
                    << "$coll");
    ASSERT_THROWS_CODE(CommandHelpers::parseNsOrUUID(
                           DatabaseName::createDatabaseName_forTest(boost::none, "test"), cmd),
                       DBException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(ParseNsOrUUID, ParseValidColl) {
    auto cmd = BSON("query"
                    << "coll");
    auto parsedNss = CommandHelpers::parseNsOrUUID(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"), cmd);
    ASSERT_EQ(parsedNss.nss(), NamespaceString::createNamespaceString_forTest("test.coll"));
}

TEST_F(ParseNsOrUUID, ParseValidCollLocalOpLogDollarMain) {
    auto cmd = BSON("query"
                    << "oplog.$main");
    auto parsedNss = CommandHelpers::parseNsOrUUID(
        DatabaseName::createDatabaseName_forTest(
            boost::none, NamespaceString::kLocalOplogDollarMain.db(omitTenant)),
        cmd);
    ASSERT_EQ(parsedNss.nss(), NamespaceString::kLocalOplogDollarMain);
}

TEST_F(ParseNsOrUUID, ParseValidUUID) {
    const UUID uuid = UUID::gen();
    auto cmd = BSON("query" << uuid);
    auto parsedNsOrUUID = CommandHelpers::parseNsOrUUID(
        DatabaseName::createDatabaseName_forTest(boost::none, "test"), cmd);
    ASSERT_EQUALS(uuid, parsedNsOrUUID.uuid());
}

MONGO_REGISTER_COMMAND(commands_test_example::ExampleIncrementCommand).forShard();

MONGO_REGISTER_COMMAND(commands_test_example::ExampleMinimalCommand).forShard();

MONGO_REGISTER_COMMAND(commands_test_example::ExampleVoidCommand).forShard();

MONGO_REGISTER_COMMAND(commands_test_example::ThrowsStatusCommand).forShard();

MONGO_REGISTER_COMMAND(commands_test_example::UnauthorizedCommand).forShard();

class TypedCommandTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        // Set up the auth subsystem to authorize the command.
        auto localManagerState = std::make_unique<AuthzManagerExternalStateMock>();
        _managerState = localManagerState.get();
        {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _managerState->setAuthzVersion(opCtx, AuthorizationManager::schemaVersion26Final);
        }
        auto uniqueAuthzManager =
            std::make_unique<AuthorizationManagerImpl>(getService(), std::move(localManagerState));
        _authzManager = uniqueAuthzManager.get();
        AuthorizationManager::set(getService(), std::move(uniqueAuthzManager));
        _authzManager->setAuthEnabled(true);

        _session = _transportLayer.createSession();
        _client = getServiceContext()->getService()->makeClient("testClient", _session);
        _registry = getCommandRegistry(_client->getService());
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

    template <typename ConcreteCommand>
    auto& fetchCommandAs(StringData name) {
        return *dynamic_cast<ConcreteCommand*>(_registry->findCommand(name));
    }

    auto& throwsStatusCommand() {
        return fetchCommandAs<commands_test_example::ThrowsStatusCommand>("throwsStatus");
    }

    auto& unauthorizedCommand() {
        return fetchCommandAs<commands_test_example::UnauthorizedCommand>("unauthorizedCmd");
    }

    auto& exampleIncrementCommand() {
        return fetchCommandAs<commands_test_example::ExampleIncrementCommand>("exampleIncrement");
    }

    auto& exampleMinimalCommand() {
        return fetchCommandAs<commands_test_example::ExampleMinimalCommand>("exampleMinimal");
    }

    auto& exampleVoidCommand() {
        return fetchCommandAs<commands_test_example::ExampleVoidCommand>("exampleVoid");
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
                return incr.serialize(BSON("$db" << ns.db_forTest()));
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
    CommandRegistry* _registry;
};

const UserName kVarunTest("varun", "test");
const UserRequest kVarunTestRequest(kVarunTest, boost::none);

TEST_F(TypedCommandTest, runTyped) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(exampleIncrementCommand(), [](int i, const BSONObj& reply) {
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

    runIncr(exampleMinimalCommand(), [](int i, const BSONObj& reply) {
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

    runIncr(exampleVoidCommand(), [&](int i, const BSONObj& reply) {
        ASSERT_EQ(reply["ok"].Double(), 1.0);
        ASSERT_EQ(exampleVoidCommand().iCapture, i + 1);
    });
}

TEST_F(TypedCommandTest, runThrowStatus) {
    {
        auto opCtx = _client->makeOperationContext();
        ASSERT_OK(_authzSession->addAndAuthorizeUser(opCtx.get(), kVarunTestRequest, boost::none));
        _authzSession->startRequest(opCtx.get());
    }

    runIncr(throwsStatusCommand(), [](int i, const BSONObj& reply) {
        Status status = Status::OK();
        try {
            uasserted(ErrorCodes::UnknownError, "some error");
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

    runIncr(unauthorizedCommand(), [](int i, const BSONObj& reply) {
        Status status = Status::OK();
        try {
            uasserted(ErrorCodes::Unauthorized, "Not authorized");
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

    runIncr(exampleIncrementCommand(), [](int i, const BSONObj& reply) {
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

    runIncr(exampleIncrementCommand(), [](int i, const BSONObj& reply) {
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

/** Support the tests' need to make "throwaway" command registrations. */
template <int n>
class TrivialNopCommand : public BasicCommand {
    static int nextSerial() {
        static int serial = 0;
        return ++serial;
    }

    static std::string makeName() {
        return "trivialNopCommand_{}_{}"_format(n, nextSerial());
    }

public:
    TrivialNopCommand() : BasicCommand{makeName()} {}
    bool run(OperationContext*, const DatabaseName&, const BSONObj&, BSONObjBuilder&) override {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj&) const override {
        return true;
    }
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }
};

using TypeInfoSet = std::set<const std::type_info*>;

TypeInfoSet getPlanEntryTypes(CommandConstructionPlan& plan) {
    TypeInfoSet res;
    for (const auto& e : plan.entries())
        ASSERT_TRUE(res.insert(e->typeInfo).second);
    return res;
}

TypeInfoSet getCommandTypes(const CommandRegistry& reg) {
    TypeInfoSet res;
    reg.forEachCommand([&](Command* cmd) { ASSERT_TRUE(res.insert(&typeid(*cmd)).second); });
    return res;
}

class CommandConstructionPlanTest : public unittest::Test {
public:
    /**
     * Populates `plan` with `count` registrations of generated Command types.
     * Returns a set of the typeids of the Commands that were registered, which
     * tests can use to make expectations.
     */
    template <size_t count>
    TypeInfoSet registerSomeUniqueNops(CommandConstructionPlan& plan) {
        TypeInfoSet typesAdded;
        auto populateOneCommand = [&]<typename Cmd>(std::type_identity<Cmd>) {
            *CommandConstructionPlan::EntryBuilder::make<Cmd>().setPlan(&plan);
            typesAdded.insert(&typeid(Cmd));
        };
        [&]<int... i>(std::integer_sequence<int, i...>) {
            (populateOneCommand(std::type_identity<TrivialNopCommand<i>>{}), ...);
        }
        (std::make_integer_sequence<int, count>{});
        return typesAdded;
    }
};

TEST_F(CommandConstructionPlanTest, BuildPlan) {
    CommandConstructionPlan plan;
    auto fullSet = registerSomeUniqueNops<2>(plan);
    ASSERT_EQ(getPlanEntryTypes(plan), fullSet);
}

TEST_F(CommandConstructionPlanTest, ExecutePlanPredicateTrue) {
    CommandConstructionPlan plan;
    auto fullSet = registerSomeUniqueNops<2>(plan);
    CommandRegistry reg;
    plan.execute(&reg, nullptr, [](auto&&) { return true; });
    ASSERT_EQ(getCommandTypes(reg), fullSet);
}

TEST_F(CommandConstructionPlanTest, ExecutePlanPredicateFalse) {
    CommandConstructionPlan plan;
    auto fullSet = registerSomeUniqueNops<2>(plan);
    CommandRegistry reg;
    plan.execute(&reg, nullptr, [](auto&&) { return false; });
    ASSERT_EQ(getCommandTypes(reg), TypeInfoSet{});
}

/** Create a ServiceContext for each supported ClusterRole mask. */
class BasicCommandRegistryTest : public ServiceContextTest {
public:
    struct ExecutePlanForServiceResult {
        TypeInfoSet fullSet;
        TypeInfoSet commandTypes;
    };

    virtual ClusterRole serverRole() const = 0;

    TypeInfoSet initPlan(CommandConstructionPlan& plan) {
        TypeInfoSet typesAdded;
        // Populate plan with a few commands, each configured for `serverRole()`.
        auto populateOneCommand = [&]<typename Cmd>(std::type_identity<Cmd>) {
            *CommandConstructionPlan::EntryBuilder::make<Cmd>()
                 .addRoles(serverRole())
                 .setPlan(&plan);
            typesAdded.insert(&typeid(Cmd));
        };
        [&]<int... i>(std::integer_sequence<int, i...>) {
            (populateOneCommand(std::type_identity<TrivialNopCommand<i>>{}), ...);
        }
        (std::make_integer_sequence<int, 3>{});
        return typesAdded;
    }

    TypeInfoSet getCommandsForRole(const CommandConstructionPlan& plan, ClusterRole role) {
        Service* service = getServiceContext()->getService(role);
        if (!service)
            return {};
        invariant(service->role().hasExclusively(role));
        CommandRegistry reg;
        plan.execute(&reg, service);
        return getCommandTypes(reg);
    }

    ExecutePlanForServiceResult testExecutePlanForService(ClusterRole sourceServiceRole) {
        ExecutePlanForServiceResult result;
        CommandConstructionPlan plan;
        result.fullSet = initPlan(plan);
        result.commandTypes = getCommandsForRole(plan, sourceServiceRole);
        return result;
    }
};

/**
 * Because ServiceContextTestSetup is a virtual base, its constructor happens
 * before an ordinary generated test class constructor can intervene.
 * To get ahead of it, we virtually inherit a `RoleOverride` to run first.
 */
template <ServerRoleIndex roleIndex>
class CommandRegistryTest : public virtual RoleOverride<roleIndex>,
                            public BasicCommandRegistryTest {
public:
    ClusterRole serverRole() const override {
        return getClusterRole(roleIndex);
    }
};

using ShardCommandRegistryTest = CommandRegistryTest<ServerRoleIndex::shard>;

TEST_F(ShardCommandRegistryTest, ServicesInit) {
    auto sc = getGlobalServiceContext();
    ASSERT(sc->getService(ClusterRole::ShardServer));
    ASSERT(!sc->getService(ClusterRole::RouterServer));
}

TEST_F(ShardCommandRegistryTest, ExecutePlanForService) {
    auto result = testExecutePlanForService(ClusterRole::ShardServer);
    ASSERT_EQ(result.commandTypes, result.fullSet);
}

using RouterCommandRegistryTest = CommandRegistryTest<ServerRoleIndex::router>;

TEST_F(RouterCommandRegistryTest, ServicesInit) {
    auto sc = getGlobalServiceContext();
    ASSERT(!sc->getService(ClusterRole::ShardServer));
    ASSERT(sc->getService(ClusterRole::RouterServer));
}

TEST_F(RouterCommandRegistryTest, ExecutePlanForService) {
    auto result = testExecutePlanForService(ClusterRole::RouterServer);
    ASSERT_EQ(result.commandTypes, result.fullSet);
}

using ShardRouterCommandRegistryTest = CommandRegistryTest<ServerRoleIndex::shardRouter>;

TEST_F(ShardRouterCommandRegistryTest, ServicesInit) {
    auto sc = getGlobalServiceContext();
    ASSERT(sc->getService(ClusterRole::ShardServer));
    ASSERT(sc->getService(ClusterRole::RouterServer));
}

TEST_F(ShardRouterCommandRegistryTest, ShardExecutePlanForService) {
    // The shard side of the shard+router ServiceContext.
    auto result = testExecutePlanForService(ClusterRole::ShardServer);
    ASSERT_EQ(result.commandTypes, result.fullSet);
}

TEST_F(ShardRouterCommandRegistryTest, RouterExecutePlanForService) {
    // The router side of the shard+router ServiceContext.
    auto result = testExecutePlanForService(ClusterRole::RouterServer);
    ASSERT_EQ(result.commandTypes, result.fullSet);
}

}  // namespace
}  // namespace mongo
