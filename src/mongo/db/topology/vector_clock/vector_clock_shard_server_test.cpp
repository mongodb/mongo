// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_document_gen.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace mongo {

/**
 * Test helper for AuthorizationSessionImpl: synthesize authenticated users without real user
 * documents or config-server-backed user acquisition.
 */
class AuthorizationSessionImplTestHelper {
public:
    /**
     * Marks 'client' as authenticated as a user with the given privileges (none by default).
     */
    static void logInAsRegularUser(Client* client, PrivilegeVector privileges = {}) {
        User user(std::make_unique<UserRequestGeneral>(UserName("regularUser", "test"),
                                                       boost::none /* roles */));
        user.setPrivileges(privileges);
        auto* as = dynamic_cast<AuthorizationSessionImpl*>(AuthorizationSession::get(client));
        as->_authenticatedUser = std::move(user);
        as->_authenticationMode = AuthorizationSession::AuthenticationMode::kConnection;
        as->_updateInternalAuthorizationState();
    }
};

namespace {

class VectorClockShardServerTest : public ShardServerTestFixture {
protected:
    VectorClockShardServerTest()
        : ShardServerTestFixture(Options{}.useMockClock(true), /*setUpMajorityReads=*/false) {}

    void setUp() override {
        ShardServerTestFixture::setUp();

        auto keysCollectionClient =
            std::make_unique<KeysCollectionClientDirect>(/*mustUseLocalReads=*/false);

        VectorClockMutable::get(getServiceContext())
            ->tickClusterTimeTo(LogicalTime(Timestamp(1, 0)));

        _keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(keysCollectionClient), Seconds(1000));
        auto validator = std::make_unique<LogicalTimeValidator>(_keyManager);
        validator->init(getServiceContext());
        LogicalTimeValidator::set(getServiceContext(), std::move(validator));
    }

    void tearDown() override {
        LogicalTimeValidator::get(getServiceContext())->shutDown();

        ShardServerTestFixture::tearDown();
    }

    void populateKeyCache() {
        LogicalTimeValidator::get(getServiceContext())
            ->enableKeyGenerator(operationContext(), true);
        _keyManager->refreshNow(operationContext());
        _keyManager->stopMonitoring();
    }

    std::shared_ptr<KeysCollectionManager> _keyManager;
};


TEST_F(VectorClockShardServerTest, TickClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0.clusterTime());

    const auto r1 = vc->tickClusterTime(1);
    const auto t1 = vc->getTime();
    ASSERT_EQ(r1, t1.clusterTime());
    ASSERT_GT(r1, t0.clusterTime());

    const auto r2 = vc->tickClusterTime(2);
    const auto t2 = vc->getTime();
    ASSERT_GT(r2, r1);
    ASSERT_GT(t2.clusterTime(), r1);
}

TEST_F(VectorClockShardServerTest, TickToClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(1, 1)));
    const auto t1 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 1)), t1.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(3, 3)));
    const auto t2 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t2.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(2, 2)));
    const auto t3 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t3.clusterTime());
}

using VectorClockShardServerTestDeathTest = VectorClockShardServerTest;
DEATH_TEST_F(VectorClockShardServerTestDeathTest, CannotTickConfigTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickConfigTime(1);
}

DEATH_TEST_F(VectorClockShardServerTestDeathTest, CannotTickToConfigTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickConfigTimeTo(LogicalTime());
}

DEATH_TEST_F(VectorClockShardServerTestDeathTest, CannotTickTopologyTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTopologyTime(1);
}

DEATH_TEST_F(VectorClockShardServerTestDeathTest, CannotTickToTopologyTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTopologyTimeTo(LogicalTime());
}

TEST_F(VectorClockShardServerTest, GossipOutInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    populateKeyCache();

    const auto clusterTime = vc->tickClusterTime(1);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob, true /*force internal*/);
    auto obj = bob.obj();

    // On shard servers, gossip out to internal clients should have $clusterTime, $configTime, and
    // $topologyTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    // No signature is attached for internal clients.
    ASSERT_FALSE(obj["$clusterTime"].Obj().hasField("signature"));
    ASSERT_TRUE(obj.hasField("$configTime"));
    ASSERT_EQ(obj["$configTime"].timestamp(), VectorClock::kInitialComponentTime.asTimestamp());
    ASSERT_TRUE(obj.hasField("$topologyTime"));
    ASSERT_EQ(obj["$topologyTime"].timestamp(), VectorClock::kInitialComponentTime.asTimestamp());
}

TEST_F(VectorClockShardServerTest, GossipOutExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    populateKeyCache();

    const auto clusterTime = vc->tickClusterTime(1);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob);
    auto obj = bob.obj();

    // On shard servers, gossip out to external clients should have $clusterTime, but not
    // $configTime or $topologyTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    // A signature is always attached for external clients. Client is authed, so it receives a dummy
    // signature.
    ASSERT_TRUE(obj["$clusterTime"].Obj().hasField("signature"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["signature"].Obj()["keyId"].Long(), 0);
    ASSERT_FALSE(obj.hasField("$configTime"));
    ASSERT_FALSE(obj.hasField("$topologyTime"));
}

TEST_F(VectorClockShardServerTest, GossipInInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand, true);

    // On shard servers, gossip in from internal clients should update $clusterTime, $configTime,
    // and $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.topologyTime().asTimestamp(), Timestamp(2, 2));

    timepointsObj = BSON("$clusterTime"
                         << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                         << "$configTime" << Timestamp(1, 1) << "$topologyTime" << Timestamp(1, 1));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand, true);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.topologyTime().asTimestamp(), Timestamp(2, 2));

    // Gossiping works without a signature, since it's treated as a dummy signature.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(3, 3) << "$topologyTime" << Timestamp(3, 3));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand, true);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.configTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.topologyTime().asTimestamp(), Timestamp(3, 3));
}

TEST_F(VectorClockShardServerTest, GossipInExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand);

    // On shard servers, gossip in from external clients should update $clusterTime, but not
    // $configTime or $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime.topologyTime(), VectorClock::kInitialComponentTime);

    timepointsObj = BSON("$clusterTime"
                         << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                         << "$configTime" << Timestamp(1, 1) << "$topologyTime" << Timestamp(1, 1));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime2.topologyTime(), VectorClock::kInitialComponentTime);

    // Gossiping works without a signature, since it's treated as a dummy signature and the test's
    // client is authorized to advance the clock.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(3, 3) << "$topologyTime" << Timestamp(3, 3));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, VectorClock::GossipSource::kClientCommand);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime3.topologyTime(), VectorClock::kInitialComponentTime);
}

/**
 * Authentication state of the gossiping client: one axis of the policy matrix below.
 */
enum class GossipAuthState {
    kAuthDisabled,     // Authentication is disabled on the server entirely.
    kUnauthenticated,  // Never authenticated, non-localhost: no localhost bypass either.
    kLocalhostBypass,  // Never authenticated, but connected from localhost.
    kInternal,         // Authenticated as internal (__system).
    kRegularUser,      // Authenticated as a regular user with no privileges.
    kSecurityToken,    // Validated security token; user not yet in the AuthorizationSession.
};

/**
 * One cell of the gossipIn() policy matrix below: client auth state x gossip source x payload
 * signature, plus the expected outcome for the clock and for TimeProofMismatch.
 */
struct GossipTestCase {
    // Unique gtest case name, used by INSTANTIATE_TEST_SUITE_P below.
    const char* name;
    GossipAuthState authState;
    VectorClock::GossipSource source;
    bool mismatchedSignature = false;  // Payload time differs from the signed time.
    bool expectAdvance = false;
    bool expectTimeProofMismatch = false;
};

// Secret for the ValidatedTenancyScope tokens in runGossipTestCase().
constexpr auto kSecurityTokenSecret = "secret";

/**
 * Auth-enabled variant of VectorClockShardServerTest: runs the policy-matrix cells below via
 * runGossipTestCase() on behalf of VectorClockShardServerGossipPolicyMatrixTest.
 */
class VectorClockShardServerAuthTest : public VectorClockShardServerTest {
protected:
    void setUp() override {
        VectorClockShardServerTest::setUp();
        populateKeyCache();
        _authzManager = AuthorizationManager::get(getServiceContext()->getService());
        _authzManager->setAuthEnabled(true);
    }

    void tearDown() override {
        _authzManager->setAuthEnabled(false);
        VectorClockShardServerTest::tearDown();
    }

    /**
     * Creates a new client from a non-localhost address that has never authenticated, so it is
     * not eligible for the localhost bypass either.
     */
    ServiceContext::UniqueClient makeUnauthenticatedClient() {
        auto session = std::make_shared<transport::MockSession>(
            HostAndPort("192.168.1.100", 20000),
            SockAddr::create("192.168.1.100", 20000, AF_UNSPEC),
            SockAddr::create("192.168.1.1", 27017, AF_UNSPEC),
            nullptr);
        return getServiceContext()->getService()->makeClient("unauthenticatedClient", session);
    }

    ServiceContext::UniqueClient makeLocalhostBypassClient() {
        auto session = std::make_shared<transport::MockSession>(
            HostAndPort("127.0.0.1", 20000),
            SockAddr::create("127.0.0.1", 20000, AF_UNSPEC),
            SockAddr::create("127.0.0.1", 27017, AF_UNSPEC),
            nullptr);
        return getServiceContext()->getService()->makeClient("localhostBypassClient", session);
    }

    /**
     * Builds the $clusterTime gossip payload for the given, genuinely signed, logical time.
     */
    static BSONObj makeSignedClusterTimeBSON(const SignedLogicalTime& signedTime) {
        BSONObjBuilder signatureBuilder;
        signedTime.getProof()->appendAsBinData(signatureBuilder, "hash");
        signatureBuilder.append("keyId", signedTime.getKeyId());
        return BSON("$clusterTime" << BSON("clusterTime" << signedTime.getTime().asTimestamp()
                                                         << "signature" << signatureBuilder.obj()));
    }

    /**
     * Runs one GossipTestCase: builds its $clusterTime payload, gossips it from a new client in
     * the given auth state, and checks the declared expectations.
     */
    void runGossipTestCase(const GossipTestCase& testCase) {
        auto vc = VectorClockMutable::get(getServiceContext());
        auto validator = LogicalTimeValidator::get(getServiceContext());

        const auto initialVectorTime = vc->getTime();
        const auto initialTime = initialVectorTime.clusterTime();
        const LogicalTime signedTimeValue(Timestamp(100, 0));

        // A real, valid signature from this server. When called for, the payload's time and the
        // signature mismatch (auth_reject_mismatching_logical_times.js).
        auto signedTime = validator->trySignLogicalTime(signedTimeValue);
        ASSERT_TRUE(signedTime.getProof());
        const LogicalTime gossipTime = testCase.mismatchedSignature
            ? LogicalTime(Timestamp(signedTimeValue.asTimestamp().getSecs() + 100, 0))
            : signedTimeValue;
        const SignedLogicalTime gossipPayload = testCase.mismatchedSignature
            ? SignedLogicalTime(gossipTime, signedTime.getProof().value(), signedTime.getKeyId())
            : signedTime;

        auto timepoints = GossipedVectorClockComponents::parse(
            makeSignedClusterTimeBSON(gossipPayload), IDLParserContext("VectorClockComponents"));

        if (testCase.authState == GossipAuthState::kAuthDisabled) {
            _authzManager->setAuthEnabled(false);
        }

        auto client = testCase.authState == GossipAuthState::kLocalhostBypass
            ? makeLocalhostBypassClient()
            : makeUnauthenticatedClient();
        auto opCtx = client->makeOperationContext();
        auto* authSession = AuthorizationSession::get(opCtx->getClient());

        switch (testCase.authState) {
            case GossipAuthState::kAuthDisabled:
            case GossipAuthState::kUnauthenticated:
                ASSERT_FALSE(authSession->isAuthenticated());
                break;
            case GossipAuthState::kLocalhostBypass:
                ASSERT_FALSE(authSession->isAuthenticated());
                // isUsingLocalhostBypass() is asserted inside the AlternativeClientRegion below:
                // the bypass check inspects the current client's remote address.
                break;
            case GossipAuthState::kInternal:
                authSession->grantInternalAuthorization();
                ASSERT_TRUE(authSession->isAuthenticated());
                break;
            case GossipAuthState::kRegularUser:
                AuthorizationSessionImplTestHelper::logInAsRegularUser(opCtx->getClient());
                ASSERT_TRUE(authSession->isAuthenticated());
                break;
            case GossipAuthState::kSecurityToken: {
                // Validated security token present, but the user is not yet installed into the
                // AuthorizationSession.
                //
                // TODO SERVER-129535: Delete this ValidatedTenancyScope setup along with
                // ValidatedTenancyScope.
                unittest::ServerParameterGuard secretGuard{"testOnlyValidatedTenancyScopeKey",
                                                           kSecurityTokenSecret};
                auto vts = auth::ValidatedTenancyScopeFactory::create(
                    UserName("tokenUser", "admin"),
                    kSecurityTokenSecret,
                    auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                    auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});
                ASSERT_TRUE(vts.hasAuthenticatedUser());
                auth::ValidatedTenancyScope::set(opCtx.get(), vts);
                ASSERT_FALSE(authSession->isAuthenticated());
                break;
            }
        }

        {
            AlternativeClientRegion clientRegion(client);
            if (testCase.authState == GossipAuthState::kLocalhostBypass) {
                ASSERT_TRUE(authSession->isUsingLocalhostBypass());
            }
            if (testCase.expectTimeProofMismatch) {
                ASSERT_THROWS_CODE(vc->gossipIn(opCtx.get(), timepoints, testCase.source),
                                   DBException,
                                   ErrorCodes::TimeProofMismatch);
            } else {
                vc->gossipIn(opCtx.get(), timepoints, testCase.source);
            }
        }

        const auto afterVectorTime = vc->getTime();
        const auto afterTime = afterVectorTime.clusterTime();
        ASSERT_EQ(afterTime, testCase.expectAdvance ? gossipTime : initialTime);
    }

private:
    AuthorizationManager* _authzManager = nullptr;
};

/**
 * The gossipIn() policy matrix: {GossipAuthState} x {GossipSource} x {payload signature} cells
 * with their expected outcome, in three classes:
 *
 *   - Trusted (advance, never TimeProofMismatch): kInternalReply gossip, and kClientCommand
 *     clients authorized to advance the clock; the signature is never validated.
 *   - Validated (advance on a valid signature, TimeProofMismatch on a mismatched one): clients
 *     that pass the unauthenticated-client gate but are not authorized to advance the clock.
 *   - Dropped (never advances, never throws): unauthenticated, non-localhost kClientCommand
 *     clients; dropped outright, since validating the signature would expose the signing key
 *     as an oracle.
 */
constexpr std::array<GossipTestCase, 9> kGossipPolicyMatrix{{
    // Auth disabled: trusted outright, since isAuthorizedToAdvanceClock() is implicitly true.
    {.name = "AuthDisabled_ClientCommand_ValidSignature",
     .authState = GossipAuthState::kAuthDisabled,
     .source = VectorClock::GossipSource::kClientCommand,
     .expectAdvance = true},

    // Internal client: authorized to advance the clock, so trusted without validation.
    {.name = "Internal_ClientCommand_ValidSignature",
     .authState = GossipAuthState::kInternal,
     .source = VectorClock::GossipSource::kClientCommand,
     .expectAdvance = true},

    // Localhost bypass: passes the gate, so a valid signature advances the clock.
    {.name = "LocalhostBypass_ClientCommand_ValidSignature",
     .authState = GossipAuthState::kLocalhostBypass,
     .source = VectorClock::GossipSource::kClientCommand,
     .expectAdvance = true},

    // Regular user: passes the gate but is not authorized to advance the clock; validated.
    {.name = "RegularUser_ClientCommand_MismatchedSignature",
     .authState = GossipAuthState::kRegularUser,
     .source = VectorClock::GossipSource::kClientCommand,
     .mismatchedSignature = true,
     .expectTimeProofMismatch = true},

    // Security token, user not yet in the AuthorizationSession: passes the gate via
    // ValidatedTenancyScope; validated.
    //
    // TODO SERVER-129535: Delete with ValidatedTenancyScope.
    {.name = "SecurityToken_ClientCommand_ValidSignature",
     .authState = GossipAuthState::kSecurityToken,
     .source = VectorClock::GossipSource::kClientCommand,
     .expectAdvance = true},
    {.name = "SecurityToken_ClientCommand_MismatchedSignature",
     .authState = GossipAuthState::kSecurityToken,
     .source = VectorClock::GossipSource::kClientCommand,
     .mismatchedSignature = true,
     .expectTimeProofMismatch = true},

    // Unauthenticated, non-localhost: kClientCommand gossip is dropped outright, never
    // validated; kInternalReply is trusted via internal RPC auth.
    {.name = "Unauthenticated_ClientCommand_ValidSignature",
     .authState = GossipAuthState::kUnauthenticated,
     .source = VectorClock::GossipSource::kClientCommand},
    {.name = "Unauthenticated_ClientCommand_MismatchedSignature",
     .authState = GossipAuthState::kUnauthenticated,
     .source = VectorClock::GossipSource::kClientCommand,
     .mismatchedSignature = true},
    {.name = "Unauthenticated_InternalReply_ValidSignature",
     .authState = GossipAuthState::kUnauthenticated,
     .source = VectorClock::GossipSource::kInternalReply,
     .expectAdvance = true},
}};

/**
 * One parameterized case per kGossipPolicyMatrix cell, e.g.
 * GossipPolicyMatrix.Unauthenticated_ClientCommand_MismatchedSignature.
 */
class VectorClockShardServerGossipPolicyMatrixTest
    : public VectorClockShardServerAuthTest,
      public testing::WithParamInterface<GossipTestCase> {};

TEST_P(VectorClockShardServerGossipPolicyMatrixTest, GossipInPolicy) {
    runGossipTestCase(GetParam());
}

INSTANTIATE_TEST_SUITE_P(GossipPolicyMatrix,
                         VectorClockShardServerGossipPolicyMatrixTest,
                         testing::ValuesIn(kGossipPolicyMatrix),
                         [](const testing::TestParamInfo<GossipTestCase>& info) {
                             return info.param.name;
                         });

class VectorClockPersistenceTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }
};

const BSONObj kVectorClockQuery = BSON("_id" << "vectorClockState");

TEST_F(VectorClockPersistenceTest, PrimaryPersistVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);

    // Persist and check that the vectorClockState document has been persisted
    vc->advanceConfigTime_forTest(LogicalTime(Timestamp(2, 2)));
    vc->advanceTopologyTime_forTest(LogicalTime(Timestamp(1, 1)));
    vc->waitForDurableConfigTime().get();
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 1);

    // Check that the vectorClockState document is still one after more persist calls
    vc->advanceConfigTime_forTest(LogicalTime(Timestamp(10, 10)));
    vc->advanceTopologyTime_forTest(LogicalTime(Timestamp(1, 1)));
    vc->waitForDurableConfigTime().get();
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 1);
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithoutExistingVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.configTime());
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.topologyTime());
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithExistingVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);
    VectorClockDocument vcd;
    vcd.setConfigTime(Timestamp(100));
    vcd.setTopologyTime(Timestamp(50));
    store.add(opCtx, vcd);

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(Timestamp(100), time.configTime().asTimestamp());
    ASSERT_EQ(Timestamp(50), time.topologyTime().asTimestamp());
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithIllegalVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);
    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kVectorClockNamespace,
                  BSON("_id" << "vectorClockState"
                             << "IllegalKey"
                             << "IllegalValue"));

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.configTime());
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.topologyTime());
}

}  // namespace
}  // namespace mongo
