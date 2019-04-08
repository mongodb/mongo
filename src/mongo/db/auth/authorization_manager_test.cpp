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

/**
 * Unit tests of the AuthorizationManager type.
 */
#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/config.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

// Construct a simple, structured X509 name equivalent to "CN=mongodb.com"
SSLX509Name buildX509Name() {
    return SSLX509Name(std::vector<std::vector<SSLX509Name::Entry>>(
        {{{kOID_CommonName.toString(), 19 /* Printable String */, "mongodb.com"}}}));
}

void setX509PeerInfo(const transport::SessionHandle& session, SSLPeerInfo info) {
    auto& sslPeerInfo = SSLPeerInfo::forSession(session);
    sslPeerInfo = info;
}

using std::vector;

class AuthorizationManagerTest : public ServiceContextTest {
public:
    virtual ~AuthorizationManagerTest() {
        if (authzManager)
            authzManager->invalidateUserCache(opCtx.get());
    }

    AuthorizationManagerTest() {
        auto localExternalState = std::make_unique<AuthzManagerExternalStateMock>();
        externalState = localExternalState.get();
        auto localAuthzManager = std::make_unique<AuthorizationManagerImpl>(
            std::move(localExternalState),
            AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{});
        authzManager = localAuthzManager.get();
        externalState->setAuthorizationManager(authzManager);
        authzManager->setAuthEnabled(true);
        AuthorizationManager::set(getServiceContext(), std::move(localAuthzManager));

        // Re-initialize the client after setting the AuthorizationManager to get an
        // AuthorizationSession.
        Client::releaseCurrent();
        Client::initThread(getThreadName(), session);
        opCtx = makeOperationContext();

        credentials = BSON("SCRAM-SHA-1"
                           << scram::Secrets<SHA1Block>::generateCredentials(
                                  "password", saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "password", saslGlobalParams.scramSHA256IterationCount.load()));
    }

    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session = transportLayer.createSession();
    AuthorizationManager* authzManager;
    AuthzManagerExternalStateMock* externalState;
    BSONObj credentials;
    ServiceContext::UniqueOperationContext opCtx;
};

TEST_F(AuthorizationManagerTest, testAcquireV2User) {


    ASSERT_OK(externalState->insertPrivilegeDocument(opCtx.get(),
                                                     BSON("_id"
                                                          << "admin.v2read"
                                                          << "user"
                                                          << "v2read"
                                                          << "db"
                                                          << "test"
                                                          << "credentials"
                                                          << credentials
                                                          << "roles"
                                                          << BSON_ARRAY(BSON("role"
                                                                             << "read"
                                                                             << "db"
                                                                             << "test"))),
                                                     BSONObj()));
    ASSERT_OK(externalState->insertPrivilegeDocument(opCtx.get(),
                                                     BSON("_id"
                                                          << "admin.v2cluster"
                                                          << "user"
                                                          << "v2cluster"
                                                          << "db"
                                                          << "admin"
                                                          << "credentials"
                                                          << credentials
                                                          << "roles"
                                                          << BSON_ARRAY(BSON("role"
                                                                             << "clusterAdmin"
                                                                             << "db"
                                                                             << "admin"))),
                                                     BSONObj()));

    auto swu = authzManager->acquireUser(opCtx.get(), UserName("v2read", "test"));
    ASSERT_OK(swu.getStatus());
    auto v2read = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2read", "test"), v2read->getName());
    ASSERT(v2read->isValid());
    RoleNameIterator roles = v2read->getRoles();
    ASSERT_EQUALS(RoleName("read", "test"), roles.next());
    ASSERT_FALSE(roles.more());
    auto privilegeMap = v2read->getPrivileges();
    auto testDBPrivilege = privilegeMap[ResourcePattern::forDatabaseName("test")];
    ASSERT(testDBPrivilege.getActions().contains(ActionType::find));
    // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure

    swu = authzManager->acquireUser(opCtx.get(), UserName("v2cluster", "admin"));
    ASSERT_OK(swu.getStatus());
    auto v2cluster = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2cluster", "admin"), v2cluster->getName());
    ASSERT(v2cluster->isValid());
    RoleNameIterator clusterRoles = v2cluster->getRoles();
    ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), clusterRoles.next());
    ASSERT_FALSE(clusterRoles.more());
    privilegeMap = v2cluster->getPrivileges();
    auto clusterPrivilege = privilegeMap[ResourcePattern::forClusterResource()];
    ASSERT(clusterPrivilege.getActions().contains(ActionType::serverStatus));
    // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
}

#ifdef MONGO_CONFIG_SSL
TEST_F(AuthorizationManagerTest, testLocalX509Authorization) {
    setX509PeerInfo(
        session,
        SSLPeerInfo(buildX509Name(), {RoleName("read", "test"), RoleName("readWrite", "test")}));

    auto swu = authzManager->acquireUser(opCtx.get(), UserName("CN=mongodb.com", "$external"));
    ASSERT_OK(swu.getStatus());
    auto x509User = std::move(swu.getValue());
    ASSERT(x509User->isValid());

    stdx::unordered_set<RoleName> expectedRoles{RoleName("read", "test"),
                                                RoleName("readWrite", "test")};
    RoleNameIterator roles = x509User->getRoles();
    stdx::unordered_set<RoleName> acquiredRoles;
    while (roles.more()) {
        acquiredRoles.insert(roles.next());
    }
    ASSERT_TRUE(expectedRoles == acquiredRoles);

    const User::ResourcePrivilegeMap& privileges = x509User->getPrivileges();
    ASSERT_FALSE(privileges.empty());
    auto privilegeIt = privileges.find(ResourcePattern::forDatabaseName("test"));
    ASSERT(privilegeIt != privileges.end());
    ASSERT(privilegeIt->second.includesAction(ActionType::insert));
}
#endif

TEST_F(AuthorizationManagerTest, testLocalX509AuthorizationInvalidUser) {
    setX509PeerInfo(
        session,
        SSLPeerInfo(buildX509Name(), {RoleName("read", "test"), RoleName("write", "test")}));

    ASSERT_NOT_OK(
        authzManager->acquireUser(opCtx.get(), UserName("CN=10gen.com", "$external")).getStatus());
}

TEST_F(AuthorizationManagerTest, testLocalX509AuthenticationNoAuthorization) {
    setX509PeerInfo(session, {});

    ASSERT_NOT_OK(authzManager->acquireUser(opCtx.get(), UserName("CN=mongodb.com", "$external"))
                      .getStatus());
}

/**
 * An implementation of AuthzManagerExternalStateMock that overrides the getUserDescription method
 * to return the user document unmodified from how it was inserted.  When using this insert user
 * documents in the format that would be returned from a usersInfo command run with
 * showPrivilges:true, rather than the format that would normally be stored in a system.users
 * collection.  The main difference between using this mock and the normal
 * AuthzManagerExternalStateMock is that with this one you should specify the 'inheritedPrivileges'
 * field in any user documents added.
 */
class AuthzManagerExternalStateMockWithExplicitUserPrivileges
    : public AuthzManagerExternalStateMock {
public:
    /**
     * This version of getUserDescription just loads the user doc directly as it was inserted into
     * the mock's user document catalog, without performing any role resolution.  This way the tests
     * can control exactly what privileges are returned for the user.
     */
    Status getUserDescription(OperationContext* opCtx,
                              const UserName& userName,
                              BSONObj* result) override {
        return _getUserDocument(opCtx, userName, result);
    }

private:
    Status _getUserDocument(OperationContext* opCtx, const UserName& userName, BSONObj* userDoc) {
        Status status = findOne(opCtx,
                                AuthorizationManager::usersCollectionNamespace,
                                BSON(AuthorizationManager::USER_NAME_FIELD_NAME
                                     << userName.getUser()
                                     << AuthorizationManager::USER_DB_FIELD_NAME
                                     << userName.getDB()),
                                userDoc);
        if (status == ErrorCodes::NoMatchingDocument) {
            status = Status(ErrorCodes::UserNotFound,
                            str::stream() << "Could not find user \"" << userName.getUser()
                                          << "\" for db \""
                                          << userName.getDB()
                                          << "\"");
        }
        return status;
    }
};

class AuthorizationManagerWithExplicitUserPrivilegesTest : public ::mongo::unittest::Test {
public:
    virtual void setUp() {
        auto localExternalState =
            stdx::make_unique<AuthzManagerExternalStateMockWithExplicitUserPrivileges>();
        externalState = localExternalState.get();
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        authzManager = stdx::make_unique<AuthorizationManagerImpl>(
            std::move(localExternalState),
            AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{});
        externalState->setAuthorizationManager(authzManager.get());
        authzManager->setAuthEnabled(true);
    }

    std::unique_ptr<AuthorizationManager> authzManager;
    AuthzManagerExternalStateMockWithExplicitUserPrivileges* externalState;
};

// Tests SERVER-21535, unrecognized actions should be ignored rather than causing errors.
TEST_F(AuthorizationManagerTest, testAcquireV2UserWithUnrecognizedActions) {


    ASSERT_OK(externalState->insertPrivilegeDocument(
        opCtx.get(),
        BSON("_id"
             << "admin.myUser"
             << "user"
             << "myUser"
             << "db"
             << "test"
             << "credentials"
             << credentials
             << "roles"
             << BSON_ARRAY(BSON("role"
                                << "myRole"
                                << "db"
                                << "test"))
             << "inheritedPrivileges"
             << BSON_ARRAY(BSON("resource" << BSON("db"
                                                   << "test"
                                                   << "collection"
                                                   << "")
                                           << "actions"
                                           << BSON_ARRAY("find"
                                                         << "fakeAction"
                                                         << "insert")))),
        BSONObj()));

    auto swu = authzManager->acquireUser(opCtx.get(), UserName("myUser", "test"));
    ASSERT_OK(swu.getStatus());
    auto myUser = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("myUser", "test"), myUser->getName());
    ASSERT(myUser->isValid());
    RoleNameIterator roles = myUser->getRoles();
    ASSERT_EQUALS(RoleName("myRole", "test"), roles.next());
    ASSERT_FALSE(roles.more());
    auto privilegeMap = myUser->getPrivileges();
    auto testDBPrivilege = privilegeMap[ResourcePattern::forDatabaseName("test")];
    ActionSet actions = testDBPrivilege.getActions();
    ASSERT(actions.contains(ActionType::find));
    ASSERT(actions.contains(ActionType::insert));
    actions.removeAction(ActionType::find);
    actions.removeAction(ActionType::insert);
    ASSERT(actions.empty());
}

// These tests ensure that the AuthorizationManager registers a
// Change on the RecoveryUnit, when an Op is reported that could
// modify role data. This Change is might recompute
// the RoleGraph when executed.
class AuthorizationManagerLogOpTest : public AuthorizationManagerTest {
public:
    class MockRecoveryUnit : public RecoveryUnitNoop {
    public:
        MockRecoveryUnit(size_t* registeredChanges) : _registeredChanges(registeredChanges) {}

        virtual void registerChange(Change* change) final {
            // RecoveryUnitNoop takes ownership of the Change
            RecoveryUnitNoop::registerChange(change);
            ++(*_registeredChanges);
        }

    private:
        size_t* _registeredChanges;
    };

    virtual void setUp() override {
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(recoveryUnit),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        AuthorizationManagerTest::setUp();
    }

    size_t registeredChanges = 0;
    MockRecoveryUnit* recoveryUnit = new MockRecoveryUnit(&registeredChanges);
};

TEST_F(AuthorizationManagerLogOpTest, testDropDatabaseAddsRecoveryUnits) {
    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("dropDatabase"
                             << "1"),
                        nullptr);
    ASSERT_EQ(size_t(1), registeredChanges);
}

TEST_F(AuthorizationManagerLogOpTest, testDropAuthCollectionAddsRecoveryUnits) {
    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("drop"
                             << "system.users"),
                        nullptr);
    ASSERT_EQ(size_t(1), registeredChanges);

    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("drop"
                             << "system.roles"),
                        nullptr);
    ASSERT_EQ(size_t(2), registeredChanges);

    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("drop"
                             << "system.version"),
                        nullptr);
    ASSERT_EQ(size_t(3), registeredChanges);

    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("drop"
                             << "system.profile"),
                        nullptr);
    ASSERT_EQ(size_t(3), registeredChanges);
}

TEST_F(AuthorizationManagerLogOpTest, testCreateAnyCollectionAddsNoRecoveryUnits) {
    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("create"
                             << "system.users"),
                        nullptr);

    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("create"
                             << "system.profile"),
                        nullptr);

    authzManager->logOp(opCtx.get(),
                        "c",
                        {"admin", "$cmd"},
                        BSON("create"
                             << "system.other"),
                        nullptr);

    ASSERT_EQ(size_t(0), registeredChanges);
}

TEST_F(AuthorizationManagerLogOpTest, testRawInsertAddsRecoveryUnits) {
    authzManager->logOp(opCtx.get(),
                        "i",
                        {"admin", "system.profile"},
                        BSON("_id"
                             << "admin.user"),
                        nullptr);
    ASSERT_EQ(size_t(0), registeredChanges);

    authzManager->logOp(opCtx.get(),
                        "i",
                        {"admin", "system.users"},
                        BSON("_id"
                             << "admin.user"),
                        nullptr);
    ASSERT_EQ(size_t(1), registeredChanges);

    authzManager->logOp(opCtx.get(),
                        "i",
                        {"admin", "system.roles"},
                        BSON("_id"
                             << "admin.user"),
                        nullptr);
    ASSERT_EQ(size_t(2), registeredChanges);
}

}  // namespace
}  // namespace mongo
