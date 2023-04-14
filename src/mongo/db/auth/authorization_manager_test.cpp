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

#include <memory>

#include "mongo/base/status.h"
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
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/ssl_peer_info.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

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

class AuthorizationManagerTest : public ServiceContextTest {
public:
    AuthorizationManagerTest() {
        auto localExternalState = std::make_unique<AuthzManagerExternalStateMock>();
        externalState = localExternalState.get();
        auto localAuthzManager = std::make_unique<AuthorizationManagerImpl>(
            getServiceContext(), std::move(localExternalState));
        authzManager = localAuthzManager.get();
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

    ~AuthorizationManagerTest() {
        if (authzManager)
            authzManager->invalidateUserCache(opCtx.get());
    }

    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
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
                                                          << "credentials" << credentials << "roles"
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
                                                          << "credentials" << credentials << "roles"
                                                          << BSON_ARRAY(BSON("role"
                                                                             << "clusterAdmin"
                                                                             << "db"
                                                                             << "admin"))),
                                                     BSONObj()));

    auto swu = authzManager->acquireUser(opCtx.get(), {{"v2read", "test"}, boost::none});
    ASSERT_OK(swu.getStatus());
    auto v2read = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2read", "test"), v2read->getName());
    ASSERT(v2read.isValid());
    RoleNameIterator roles = v2read->getRoles();
    ASSERT_EQUALS(RoleName("read", "test"), roles.next());
    ASSERT_FALSE(roles.more());
    auto privilegeMap = v2read->getPrivileges();
    auto testDBPrivilege = privilegeMap[ResourcePattern::forDatabaseName("test")];
    ASSERT(testDBPrivilege.getActions().contains(ActionType::find));
    // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure

    swu = authzManager->acquireUser(opCtx.get(), {{"v2cluster", "admin"}, boost::none});
    ASSERT_OK(swu.getStatus());
    auto v2cluster = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("v2cluster", "admin"), v2cluster->getName());
    ASSERT(v2cluster.isValid());
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
    std::set<RoleName> roles({{"read", "test"}, {"readWrite", "test"}});
    UserRequest request(UserName("CN=mongodb.com", "$external"), roles);

    auto swu = authzManager->acquireUser(opCtx.get(), request);
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
    auto privilegeIt = privileges.find(ResourcePattern::forDatabaseName("test"));
    ASSERT(privilegeIt != privileges.end());
    ASSERT(privilegeIt->second.includesAction(ActionType::insert));
}

TEST_F(AuthorizationManagerTest, testLocalX509AuthorizationInvalidUser) {
    setX509PeerInfo(session,
                    SSLPeerInfo(buildX509Name(),
                                boost::none,
                                {RoleName("read", "test"), RoleName("write", "test")}));

    ASSERT_NOT_OK(
        authzManager->acquireUser(opCtx.get(), {{"CN=10gen.com", "$external"}, boost::none})
            .getStatus());
}

TEST_F(AuthorizationManagerTest, testLocalX509AuthenticationNoAuthorization) {
    setX509PeerInfo(session, {});

    ASSERT_NOT_OK(
        authzManager->acquireUser(opCtx.get(), {{"CN=mongodb.com", "$external"}, boost::none})
            .getStatus());
}

#endif

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
             << "credentials" << credentials << "roles"
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

    auto swu = authzManager->acquireUser(opCtx.get(), {{"myUser", "test"}, boost::none});
    ASSERT_OK(swu.getStatus());
    auto myUser = std::move(swu.getValue());
    ASSERT_EQUALS(UserName("myUser", "test"), myUser->getName());
    ASSERT(myUser.isValid());
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
        ASSERT_OK(externalState->insertPrivilegeDocument(opCtx.get(), userDoc, BSONObj()));
    }

    // Acquire these users to force the AuthorizationManager to load these users into the user
    // cache. Store the users into a vector so that they are checked out.
    std::vector<UserHandle> checkedOutUsers;
    checkedOutUsers.reserve(userDocs.size());
    for (const auto& userDoc : userDocs) {
        const UserName userName(userDoc.getStringField(kUserFieldName),
                                userDoc.getStringField(kDbFieldName));
        auto swUser = authzManager->acquireUser(opCtx.get(), {userName, boost::none});
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
            externalState->updateOne(opCtx.get(),
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
        if (checkedOutUser->getName().getDB() == DatabaseName::kExternal.db()) {
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
        auto swUser = authzManager->acquireUser(opCtx.get(), {userName, boost::none});
        ASSERT_OK(swUser.getStatus());
        auto user = std::move(swUser.getValue());
        ASSERT_EQUALS(userName, user->getName());
        ASSERT(user.isValid());

        RoleNameIterator cachedUserRolesIt = user->getRoles();
        if (userDoc.getStringField(kDbFieldName) == DatabaseName::kExternal.db()) {
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

}  // namespace
}  // namespace mongo
