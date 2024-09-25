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

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/read_through_cache.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

using ResolveRoleOption = AuthorizationManager::ResolveRoleOption;

#ifdef MONGO_CONFIG_SSL
// Construct a simple, structured X509 name equivalent to "CN=mongodb.com"
SSLX509Name buildX509Name() {
    return SSLX509Name(std::vector<std::vector<SSLX509Name::Entry>>(
        {{{kOID_CommonName.toString(), 19 /* Printable String */, "mongodb.com"}}}));
}

void setX509PeerInfo(const std::shared_ptr<transport::Session>& session, SSLPeerInfo info) {
    auto& sslPeerInfo = SSLPeerInfo::forSession(session);
    sslPeerInfo = info;
}

#endif

const auto kTestDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);
const auto kTestRsrc = ResourcePattern::forDatabaseName(kTestDB);

// Custom AuthorizationManagerImpl which keeps track of how many times user cache invalidation
// functions have been called.
class AuthorizationManagerImplForTest : public AuthorizationManagerImpl {
public:
    using AuthorizationManagerImpl::AuthorizationManagerImpl;

    void invalidateUserByName(const UserName& user) override {
        _byNameCount.fetchAndAdd(1);
        AuthorizationManagerImpl::invalidateUserByName(user);
    }

    void invalidateUsersByTenant(const boost::optional<TenantId>& tenant) override {
        _byTenantCount.fetchAndAdd(1);
        AuthorizationManagerImpl::invalidateUsersByTenant(tenant);
    }

    void invalidateUserCache() override {
        _wholeCacheCount.fetchAndAdd(1);
        AuthorizationManagerImpl::invalidateUserCache();
    }

    void resetCounts() {
        _byNameCount.store(0);
        _byTenantCount.store(0);
        _wholeCacheCount.store(0);
    }

    void assertCounts(uint64_t whole, uint64_t name, uint64_t tenant) {
        ASSERT_EQ(whole, _wholeCacheCount.load());
        ASSERT_EQ(name, _byNameCount.load());
        ASSERT_EQ(tenant, _byTenantCount.load());
    }

private:
    AtomicWord<uint64_t> _byNameCount = 0, _byTenantCount = 0, _wholeCacheCount = 0;
};

// Custom RecoveryUnit which extends RecoveryUnitNoop to handle entering and exiting WUOWs. Does not
// work for nested WUOWs.
class RecoveryUnitMock : public RecoveryUnitNoop {
    void doBeginUnitOfWork() override {
        _setState(State::kActive);
    }

    void doCommitUnitOfWork() override {
        _executeCommitHandlers(boost::none);
        _setState(State::kActiveNotInUnitOfWork);
    }

    void doAbortUnitOfWork() override {
        _executeRollbackHandlers();
        _setState(State::kActiveNotInUnitOfWork);
    }
};

class AuthorizationManagerTest : public ServiceContextTest {
public:
    AuthorizationManagerTest() {
        auto externalStateWrapper = std::make_unique<AuthzManagerExternalStateMock>();
        externalState = externalStateWrapper.get();
        auto localAuthzManager = std::make_unique<AuthorizationManagerImplForTest>(
            getService(), std::move(externalStateWrapper));
        authzManager = localAuthzManager.get();
        authzManager->setAuthEnabled(true);
        AuthorizationManager::set(getService(), std::move(localAuthzManager));
        auth::AuthorizationBackendInterface::set(
            getService(), std::make_unique<auth::AuthorizationBackendMock>());
        mockBackend = reinterpret_cast<auth::AuthorizationBackendMock*>(
            auth::AuthorizationBackendInterface::get(getService()));

        // Re-initialize the client after setting the AuthorizationManager to get an
        // AuthorizationSession.
        Client::releaseCurrent();
        Client::initThread(getThreadName(), getServiceContext()->getService(), session);
        opCtx = makeOperationContext();

        credentials = BSON("SCRAM-SHA-1"
                           << scram::Secrets<SHA1Block>::generateCredentials(
                                  "password", saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "password", saslGlobalParams.scramSHA256IterationCount.load()));

        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            std::make_unique<RecoveryUnitMock>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }

    ~AuthorizationManagerTest() override {
        if (authzManager)
            AuthorizationManager::get(opCtx->getService())->invalidateUserCache();
    }

    void resetInvalidateCounts() {
        authzManager->resetCounts();
    }

    void assertInvalidateCounts(uint64_t whole, uint64_t name, uint64_t tenant) {
        authzManager->assertCounts(whole, name, tenant);
    }

    // Helpers that log ops which should cause a specific type of user invalidation.
    void logOpSingleUser() {
        externalState->logOp(opCtx.get(),
                             authzManager,
                             "i"_sd,
                             NamespaceString::kAdminUsersNamespace,
                             BSON("_id"
                                  << "db.user"),
                             NULL);
    }

    void logOpWholeCache() {
        externalState->logOp(opCtx.get(),
                             authzManager,
                             "i"_sd,
                             NamespaceString::kAdminUsersNamespace,
                             BSON("_id"
                                  << "invaliduser"),
                             NULL);
    }

    void logOpTenant() {
        TenantId tenantId(OID::gen());
        auto ns = NamespaceString::makeTenantUsersCollection(tenantId);
        externalState->logOp(opCtx.get(),
                             authzManager,
                             ""_sd,
                             ns,
                             BSON("_id"
                                  << "db.user"),
                             NULL);
    }

    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
    AuthorizationManagerImplForTest* authzManager;
    AuthzManagerExternalStateMock* externalState;
    auth::AuthorizationBackendMock* mockBackend;
    BSONObj credentials;
    ServiceContext::UniqueOperationContext opCtx;
};

TEST_F(AuthorizationManagerTest, testAcquireV2User) {
    ASSERT_OK(mockBackend->insertUserDocument(opCtx.get(),
                                              BSON("_id"
                                                   << "admin.v2read"
                                                   << "user"
                                                   << "v2read"
                                                   << "db"
                                                   << "test"
                                                   << "credentials" << credentials << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "read"
                                                                      << "db"
                                                                      << "test"))),
                                              BSONObj()));
    ASSERT_OK(mockBackend->insertUserDocument(opCtx.get(),
                                              BSON("_id"
                                                   << "admin.v2cluster"
                                                   << "user"
                                                   << "v2cluster"
                                                   << "db"
                                                   << "admin"
                                                   << "credentials" << credentials << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "clusterAdmin"
                                                                      << "db"
                                                                      << "admin"))),
                                              BSONObj()));

    auto swu = authzManager->acquireUser(
        opCtx.get(), std::make_unique<UserRequestGeneral>(UserName("v2read", "test"), boost::none));
    ASSERT_OK(swu.getStatus());
    auto v2read = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2read", "test"), v2read->getName());
    ASSERT(v2read.isValid());
    RoleNameIterator roles = v2read->getRoles();
    ASSERT_EQUALS(RoleName("read", "test"), roles.next());
    ASSERT_FALSE(roles.more());
    auto privilegeMap = v2read->getPrivileges();
    auto testDBPrivilege = privilegeMap[kTestRsrc];
    ASSERT(testDBPrivilege.getActions().contains(ActionType::find));
    // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure

    swu = authzManager->acquireUser(
        opCtx.get(),
        std::make_unique<UserRequestGeneral>(UserName("v2cluster", "admin"), boost::none));
    ASSERT_OK(swu.getStatus());
    auto v2cluster = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2cluster", "admin"), v2cluster->getName());
    ASSERT(v2cluster.isValid());
    RoleNameIterator clusterRoles = v2cluster->getRoles();
    ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), clusterRoles.next());
    ASSERT_FALSE(clusterRoles.more());
    privilegeMap = v2cluster->getPrivileges();
    auto clusterPrivilege = privilegeMap[ResourcePattern::forClusterResource(boost::none)];
    ASSERT(clusterPrivilege.getActions().contains(ActionType::serverStatus));
    // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
}

#ifdef MONGO_CONFIG_SSL
TEST_F(AuthorizationManagerTest, testLocalX509Authorization) {
    std::set<RoleName> roles({{"read", "test"}, {"readWrite", "test"}});
    std::unique_ptr<UserRequest> request =
        std::make_unique<UserRequestGeneral>(UserName("CN=mongodb.com", "$external"), roles);

    auto swu = authzManager->acquireUser(opCtx.get(), std::move(request));
    ASSERT_OK(swu.getStatus());
    auto x509User = std::move(swu.getValue());
    ASSERT(x509User.isValid());

    std::set<RoleName> gotRoles;
    for (auto it = x509User->getRoles(); it.more();) {
        gotRoles.emplace(it.next());
    }
    ASSERT_TRUE(roles == gotRoles);

    const User::ResourcePrivilegeMap& privileges = x509User->getPrivileges();
    ASSERT_FALSE(privileges.empty());
    auto privilegeIt = privileges.find(kTestRsrc);
    ASSERT(privilegeIt != privileges.end());
    ASSERT(privilegeIt->second.includesAction(ActionType::insert));
}

TEST_F(AuthorizationManagerTest, testLocalX509AuthorizationInvalidUser) {
    setX509PeerInfo(session,
                    SSLPeerInfo(buildX509Name(),
                                boost::none,
                                {RoleName("read", "test"), RoleName("write", "test")}));

    ASSERT_NOT_OK(authzManager
                      ->acquireUser(opCtx.get(),
                                    std::make_unique<UserRequestGeneral>(
                                        UserName("CN=10gen.com", "$external"), boost::none))
                      .getStatus());
}

TEST_F(AuthorizationManagerTest, testLocalX509AuthenticationNoAuthorization) {
    setX509PeerInfo(session, {});

    ASSERT_NOT_OK(authzManager
                      ->acquireUser(opCtx.get(),
                                    std::make_unique<UserRequestGeneral>(
                                        UserName("CN=mongodb.com", "$external"), boost::none))
                      .getStatus());
}

#endif

// Tests SERVER-21535, unrecognized actions should be ignored rather than causing errors.
TEST_F(AuthorizationManagerTest, testAcquireV2UserWithUnrecognizedActions) {
    ASSERT_OK(mockBackend->insertUserDocument(opCtx.get(),
                                              BSON("_id"
                                                   << "admin.myUser"
                                                   << "user"
                                                   << "myUser"
                                                   << "db"
                                                   << "test"
                                                   << "credentials" << credentials << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "myRole"
                                                                      << "db"
                                                                      << "test"))
                                                   << "inheritedPrivileges"
                                                   << BSON_ARRAY(BSON("resource"
                                                                      << BSON("db"
                                                                              << "test"
                                                                              << "collection"
                                                                              << "")
                                                                      << "actions"
                                                                      << BSON_ARRAY("find"
                                                                                    << "fakeAction"
                                                                                    << "insert")))),
                                              BSONObj()));

    auto swu = authzManager->acquireUser(
        opCtx.get(), std::make_unique<UserRequestGeneral>(UserName("myUser", "test"), boost::none));
    ASSERT_OK(swu.getStatus());
    auto myUser = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("myUser", "test"), myUser->getName());
    ASSERT(myUser.isValid());
    RoleNameIterator roles = myUser->getRoles();
    ASSERT_EQUALS(RoleName("myRole", "test"), roles.next());
    ASSERT_FALSE(roles.more());
    auto privilegeMap = myUser->getPrivileges();
    auto testDBPrivilege = privilegeMap[kTestRsrc];
    ActionSet actions = testDBPrivilege.getActions();
    ASSERT(actions.contains(ActionType::find));
    ASSERT(actions.contains(ActionType::insert));
    actions.removeAction(ActionType::find);
    actions.removeAction(ActionType::insert);
    ASSERT(actions.empty());
}

TEST_F(AuthorizationManagerTest, testRefreshExternalV2User) {
    constexpr auto kUserFieldName = "user"_sd;
    constexpr auto kDbFieldName = "db"_sd;
    constexpr auto kRoleFieldName = "role"_sd;

    // Insert one user on db test and two users on db $external.
    BSONObj externalCredentials = BSON("external" << true);
    std::vector<BSONObj> userDocs{BSON("_id"
                                       << "admin.v2read"
                                       << "user"
                                       << "v2read"
                                       << "db"
                                       << "test"
                                       << "credentials" << credentials << "roles"
                                       << BSON_ARRAY(BSON("role"
                                                          << "read"
                                                          << "db"
                                                          << "test"))),
                                  BSON("_id"
                                       << "admin.v2externalOne"
                                       << "user"
                                       << "v2externalOne"
                                       << "db"
                                       << "$external"
                                       << "credentials" << externalCredentials << "roles"
                                       << BSON_ARRAY(BSON("role"
                                                          << "read"
                                                          << "db"
                                                          << "test"))),
                                  BSON("_id"
                                       << "admin.v2externalTwo"
                                       << "user"
                                       << "v2externalTwo"
                                       << "db"
                                       << "$external"
                                       << "credentials" << externalCredentials << "roles"
                                       << BSON_ARRAY(BSON("role"
                                                          << "read"
                                                          << "db"
                                                          << "test")))};

    std::vector<BSONObj> initialRoles{BSON("role"
                                           << "read"
                                           << "db"
                                           << "test")};
    std::vector<BSONObj> updatedRoles{BSON("role"
                                           << "readWrite"
                                           << "db"
                                           << "test")};

    for (const auto& userDoc : userDocs) {
        ASSERT_OK(mockBackend->insertUserDocument(opCtx.get(), userDoc, BSONObj()));
    }

    // Acquire these users to force the AuthorizationManager to load these users into the user
    // cache. Store the users into a vector so that they are checked out.
    std::vector<UserHandle> checkedOutUsers;
    checkedOutUsers.reserve(userDocs.size());
    for (const auto& userDoc : userDocs) {
        const UserName userName(userDoc.getStringField(kUserFieldName),
                                userDoc.getStringField(kDbFieldName));
        auto swUser = authzManager->acquireUser(
            opCtx.get(), std::make_unique<UserRequestGeneral>(userName, boost::none));
        ASSERT_OK(swUser.getStatus());
        auto user = std::move(swUser.getValue());
        ASSERT_EQUALS(userName, user->getName());
        ASSERT(user.isValid());

        RoleNameIterator cachedUserRoles = user->getRoles();
        for (const auto& userDocRole : initialRoles) {
            ASSERT_EQUALS(cachedUserRoles.next(),
                          RoleName(userDocRole.getStringField(kRoleFieldName),
                                   userDocRole.getStringField(kDbFieldName)));
        }
        ASSERT_FALSE(cachedUserRoles.more());
        checkedOutUsers.push_back(std::move(user));
    }

    // Update each of the users added into the external state so that they gain the readWrite role.
    for (const auto& userDoc : userDocs) {
        BSONObj updateQuery = BSON("user" << userDoc.getStringField(kUserFieldName));
        ASSERT_OK(
            mockBackend->updateOne(opCtx.get(),
                                   NamespaceString::kAdminUsersNamespace,
                                   updateQuery,
                                   BSON("$set" << BSON("roles" << BSON_ARRAY(updatedRoles[0]))),
                                   true,
                                   BSONObj()));
    }

    // Refresh all external entries in the authorization manager's cache.
    ASSERT_OK(authzManager->refreshExternalUsers(opCtx.get()));

    // Assert that all checked-out $external users are now marked invalid.
    for (const auto& checkedOutUser : checkedOutUsers) {
        if (checkedOutUser->getName().getDatabaseName().isExternalDB()) {
            ASSERT(!checkedOutUser.isValid());
        } else {
            ASSERT(checkedOutUser.isValid());
        }
    }

    // Retrieve all users from the cache and verify that only the external ones contain the newly
    // added role.
    for (const auto& userDoc : userDocs) {
        const UserName userName(userDoc.getStringField(kUserFieldName),
                                userDoc.getStringField(kDbFieldName));
        auto swUser = authzManager->acquireUser(
            opCtx.get(), std::make_unique<UserRequestGeneral>(userName, boost::none));
        ASSERT_OK(swUser.getStatus());
        auto user = std::move(swUser.getValue());
        ASSERT_EQUALS(userName, user->getName());
        ASSERT(user.isValid());

        RoleNameIterator cachedUserRolesIt = user->getRoles();
        if (userDoc.getStringField(kDbFieldName) == DatabaseName::kExternal.db(omitTenant)) {
            for (const auto& userDocRole : updatedRoles) {
                ASSERT_EQUALS(cachedUserRolesIt.next(),
                              RoleName(userDocRole.getStringField(kRoleFieldName),
                                       userDocRole.getStringField(kDbFieldName)));
            }
        } else {
            for (const auto& userDocRole : initialRoles) {
                ASSERT_EQUALS(cachedUserRolesIt.next(),
                              RoleName(userDocRole.getStringField(kRoleFieldName),
                                       userDocRole.getStringField(kDbFieldName)));
            }
        }
        ASSERT_FALSE(cachedUserRolesIt.more());
    }
}

TEST_F(AuthorizationManagerTest, testLogWholeCacheInvalidateNoWuow) {
    resetInvalidateCounts();
    // With no write unit enabled, logOp should immediately push cache invalidates.
    logOpWholeCache();
    assertInvalidateCounts(1, 0, 0);
}
TEST_F(AuthorizationManagerTest, testLogWholeCacheInvalidateInWuowCommit) {
    resetInvalidateCounts();
    // With a write unit, cache invalidation should not occur until a commit().
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpWholeCache();
        assertInvalidateCounts(0, 0, 0);
        wuow.commit();
        assertInvalidateCounts(1, 0, 0);
    }
    assertInvalidateCounts(1, 0, 0);
}
TEST_F(AuthorizationManagerTest, testLogWholeCacheInvalidateInWuowAbandon) {
    resetInvalidateCounts();
    // If the write unit is abandoned (which we can emulate by leaving its owned scope without
    // committing), cache invalidation should not occur.
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpWholeCache();
        assertInvalidateCounts(0, 0, 0);
    }
    assertInvalidateCounts(0, 0, 0);
}

TEST_F(AuthorizationManagerTest, testLogSpecificUserInvalidateNoWuow) {
    resetInvalidateCounts();
    // With no write unit enabled, logOp should immediately push cache invalidates.
    logOpSingleUser();
    assertInvalidateCounts(0, 1, 0);
}
TEST_F(AuthorizationManagerTest, testLogSpecificUserInvalidateInWuowCommit) {
    resetInvalidateCounts();
    // With a write unit, cache invalidation should not occur until a commit().
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpSingleUser();
        assertInvalidateCounts(0, 0, 0);
        wuow.commit();
        assertInvalidateCounts(0, 1, 0);
    }
    assertInvalidateCounts(0, 1, 0);
}
TEST_F(AuthorizationManagerTest, testLogSpecificUserInvalidateInWuowAbandon) {
    resetInvalidateCounts();
    // If the write unit is abandoned (which we can emulate by leaving its owned scope without
    // committing), cache invalidation should not occur.
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpSingleUser();
        assertInvalidateCounts(0, 0, 0);
    }
    assertInvalidateCounts(0, 0, 0);
}

TEST_F(AuthorizationManagerTest, testLogTenantInvalidateNoWuow) {
    resetInvalidateCounts();
    // With no write unit enabled, logOp should immediately push cache invalidates.
    logOpTenant();
    assertInvalidateCounts(0, 0, 1);
}
TEST_F(AuthorizationManagerTest, testLogTenantInvalidateInWuowCommit) {
    resetInvalidateCounts();
    // With a write unit, cache invalidation should not occur until a commit().
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpTenant();
        assertInvalidateCounts(0, 0, 0);
        wuow.commit();
        assertInvalidateCounts(0, 0, 1);
    }
    assertInvalidateCounts(0, 0, 1);
}
TEST_F(AuthorizationManagerTest, testLogTenantInvalidateInWuowAbandon) {
    resetInvalidateCounts();
    // If the write unit is abandoned (which we can emulate by leaving its owned scope without
    // committing), cache invalidation should not occur.
    {
        WriteUnitOfWork wuow(opCtx.get());
        logOpTenant();
        assertInvalidateCounts(0, 0, 0);
    }
    assertInvalidateCounts(0, 0, 0);
}

TEST_F(AuthorizationManagerTest, testAuthzManagerExternalStateResolveRoles) {
    BSONObj innerCustomRole{BSON("_id"
                                 << "admin.innerTestRole"
                                 << "db"
                                 << "admin"
                                 << "role"
                                 << "innerTestRole"
                                 << "roles"
                                 << BSON_ARRAY(BSON("db"
                                                    << "test"
                                                    << "role"
                                                    << "read")))};
    BSONObj middleCustomRole{BSON("_id"
                                  << "admin.middleTestRole"
                                  << "db"
                                  << "admin"
                                  << "role"
                                  << "middleTestRole"
                                  << "roles" << BSON_ARRAY(innerCustomRole))};
    BSONObj outerCustomRole{BSON("_id"
                                 << "admin.outerTestRole"
                                 << "db"
                                 << "admin"
                                 << "role"
                                 << "outerTestRole"
                                 << "roles" << BSON_ARRAY(middleCustomRole) << "privileges"
                                 << BSON_ARRAY(BSON("resource" << BSON("cluster" << true)
                                                               << "actions"
                                                               << BSON_ARRAY("shutdown")))
                                 << "authenticationRestrictions"
                                 << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("127.0.0.1/8"))))};
    std::vector<BSONObj> customRoleDocuments{innerCustomRole, middleCustomRole, outerCustomRole};

    for (const auto& roleDoc : customRoleDocuments) {
        ASSERT_OK(mockBackend->insertRoleDocument(opCtx.get(), roleDoc, BSONObj()));
    }

    // First, resolve all roles (but no privileges or restrictions).
    ResolveRoleOption option{ResolveRoleOption::kRoles()};
    std::vector<RoleName> roleNames{RoleName{"outerTestRole", "admin"}};
    auto swResolvedData = mockBackend->resolveRoles(opCtx.get(), roleNames, option);
    ASSERT_OK(swResolvedData.getStatus());
    auto resolvedData = swResolvedData.getValue();
    ASSERT_TRUE(resolvedData.roles.has_value());
    ASSERT_EQ(resolvedData.roles->size(), 3);
    ASSERT_FALSE(resolvedData.privileges.has_value());
    ASSERT_FALSE(resolvedData.restrictions.has_value());

    // Resolve privileges and roles, no restrictions.
    option.setPrivileges(true /* shouldEnable */);
    swResolvedData = mockBackend->resolveRoles(opCtx.get(), roleNames, option);
    ASSERT_OK(swResolvedData.getStatus());
    resolvedData = swResolvedData.getValue();
    ASSERT_TRUE(resolvedData.roles.has_value());
    ASSERT_EQ(resolvedData.roles->size(), 3);
    ASSERT_TRUE(resolvedData.privileges.has_value());
    ASSERT_FALSE(resolvedData.restrictions.has_value());

    // Resolve all roles, privileges, and restrictions.
    option.setRestrictions(true /* shouldEnable */);
    swResolvedData = mockBackend->resolveRoles(opCtx.get(), roleNames, option);
    ASSERT_OK(swResolvedData.getStatus());
    resolvedData = swResolvedData.getValue();
    ASSERT_TRUE(resolvedData.roles.has_value());
    ASSERT_EQ(resolvedData.roles->size(), 3);
    ASSERT_TRUE(resolvedData.privileges.has_value());
    ASSERT_TRUE(resolvedData.restrictions.has_value());

    // Resolve only direct roles, privileges, and restrictions.
    option.setDirectOnly(true /* shouldEnable */);
    swResolvedData = mockBackend->resolveRoles(opCtx.get(), roleNames, option);
    ASSERT_OK(swResolvedData.getStatus());
    resolvedData = swResolvedData.getValue();
    ASSERT_TRUE(resolvedData.roles.has_value());
    ASSERT_EQ(resolvedData.roles->size(), 1);
    ASSERT_TRUE(resolvedData.privileges.has_value());
    ASSERT_TRUE(resolvedData.restrictions.has_value());
}

}  // namespace
}  // namespace mongo
