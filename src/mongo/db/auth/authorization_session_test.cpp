/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

/**
 * Unit tests of the AuthorizationSession type.
 */
#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

class FailureCapableAuthzManagerExternalStateMock : public AuthzManagerExternalStateMock {
public:
    FailureCapableAuthzManagerExternalStateMock() : _findsShouldFail(false) {}
    virtual ~FailureCapableAuthzManagerExternalStateMock() {}

    void setFindsShouldFail(bool enable) {
        _findsShouldFail = enable;
    }

    virtual Status findOne(OperationContext* opCtx,
                           const NamespaceString& collectionName,
                           const BSONObj& query,
                           BSONObj* result) {
        if (_findsShouldFail && collectionName == AuthorizationManager::usersCollectionNamespace) {
            return Status(ErrorCodes::UnknownError,
                          "findOne on admin.system.users set to fail in mock.");
        }
        return AuthzManagerExternalStateMock::findOne(opCtx, collectionName, query, result);
    }

private:
    bool _findsShouldFail;
};

class AuthorizationSessionTest : public ::mongo::unittest::Test {
public:
    FailureCapableAuthzManagerExternalStateMock* managerState;
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session;
    ServiceContextNoop serviceContext;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext _opCtx;
    AuthzSessionExternalStateMock* sessionState;
    AuthorizationManager* authzManager;
    std::unique_ptr<AuthorizationSessionForTest> authzSession;

    void setUp() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k36);
        session = transportLayer.createSession();
        client = serviceContext.makeClient("testClient", session);
        RestrictionEnvironment::set(
            session, stdx::make_unique<RestrictionEnvironment>(SockAddr(), SockAddr()));
        _opCtx = client->makeOperationContext();
        auto localManagerState = stdx::make_unique<FailureCapableAuthzManagerExternalStateMock>();
        managerState = localManagerState.get();
        managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        auto uniqueAuthzManager =
            stdx::make_unique<AuthorizationManager>(std::move(localManagerState));
        authzManager = uniqueAuthzManager.get();
        AuthorizationManager::set(&serviceContext, std::move(uniqueAuthzManager));
        auto localSessionState = stdx::make_unique<AuthzSessionExternalStateMock>(authzManager);
        sessionState = localSessionState.get();
        authzSession = stdx::make_unique<AuthorizationSessionForTest>(std::move(localSessionState));
        authzManager->setAuthEnabled(true);
    }
};

const NamespaceString testFooNss("test.foo");
const NamespaceString testBarNss("test.bar");
const NamespaceString testQuxNss("test.qux");

const ResourcePattern testDBResource(ResourcePattern::forDatabaseName("test"));
const ResourcePattern otherDBResource(ResourcePattern::forDatabaseName("other"));
const ResourcePattern adminDBResource(ResourcePattern::forDatabaseName("admin"));
const ResourcePattern testFooCollResource(ResourcePattern::forExactNamespace(testFooNss));
const ResourcePattern testBarCollResource(ResourcePattern::forExactNamespace(testBarNss));
const ResourcePattern testQuxCollResource(ResourcePattern::forExactNamespace(testQuxNss));
const ResourcePattern otherFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.foo")));
const ResourcePattern thirdFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.foo")));
const ResourcePattern adminFooCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("admin.foo")));
const ResourcePattern testUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.users")));
const ResourcePattern otherUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.users")));
const ResourcePattern thirdUsersCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.users")));
const ResourcePattern testIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.indexes")));
const ResourcePattern otherIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.indexes")));
const ResourcePattern thirdIndexesCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.indexes")));
const ResourcePattern testProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.profile")));
const ResourcePattern otherProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.profile")));
const ResourcePattern thirdProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.profile")));
const ResourcePattern testSystemNamespacesResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.namespaces")));

TEST_F(AuthorizationSessionTest, AddUserAndCheckAuthorization) {
    // Check that disabling auth checks works
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    sessionState->setReturnValueForShouldIgnoreAuthChecks(true);
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    sessionState->setReturnValueForShouldIgnoreAuthChecks(false);
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    // Check that you can't authorize a user that doesn't exist.
    ASSERT_EQUALS(ErrorCodes::UserNotFound,
                  authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    // Add a user with readWrite and dbAdmin on the test DB
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));

    // Add an admin user with readWriteAnyDatabase
    ASSERT_OK(
        managerState->insertPrivilegeDocument(_opCtx.get(),
                                              BSON("user"
                                                   << "admin"
                                                   << "db"
                                                   << "admin"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "readWriteAnyDatabase"
                                                                      << "db"
                                                                      << "admin"))),
                                              BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("admin", "admin")));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        ResourcePattern::forExactNamespace(NamespaceString("anydb.somecollection")),
        ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherDBResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::collMod));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    authzSession->logoutDatabase("test");
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));

    authzSession->logoutDatabase("admin");
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));
}

TEST_F(AuthorizationSessionTest, DuplicateRolesOK) {
    // Add a user with doubled-up readWrite and single dbAdmin on the test DB
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test")
                                                                       << BSON("role"
                                                                               << "readWrite"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
}

TEST_F(AuthorizationSessionTest, SystemCollectionsAccessControl) {
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "rw"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test")
                                                                       << BSON("role"
                                                                               << "dbAdmin"
                                                                               << "db"
                                                                               << "test"))),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "useradmin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "userAdmin"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(
        managerState->insertPrivilegeDocument(_opCtx.get(),
                                              BSON("user"
                                                   << "rwany"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "readWriteAnyDatabase"
                                                                      << "db"
                                                                      << "admin")
                                                                 << BSON("role"
                                                                         << "dbAdminAnyDatabase"
                                                                         << "db"
                                                                         << "admin"))),
                                              BSONObj()));
    ASSERT_OK(
        managerState->insertPrivilegeDocument(_opCtx.get(),
                                              BSON("user"
                                                   << "useradminany"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "userAdminAnyDatabase"
                                                                      << "db"
                                                                      << "admin"))),
                                              BSONObj()));

    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("rwany", "test")));

    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));

    // Logging in as useradminany@test implicitly logs out rwany@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("useradminany", "test")));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));

    // Logging in as rw@test implicitly logs out useradminany@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("rw", "test")));

    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));


    // Logging in as useradmin@test implicitly logs out rw@test.
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("useradmin", "test")));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherUsersCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherIndexesCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherProfileCollResource, ActionType::find));
}

TEST_F(AuthorizationSessionTest, InvalidateUser) {
    // Add a readWrite user
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Change the user to be read-only
    int ignored;
    managerState
        ->remove(_opCtx.get(),
                 AuthorizationManager::usersCollectionNamespace,
                 BSONObj(),
                 BSONObj(),
                 &ignored)
        .transitional_ignore();
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Delete the user.
    managerState
        ->remove(_opCtx.get(),
                 AuthorizationManager::usersCollectionNamespace,
                 BSONObj(),
                 BSONObj(),
                 &ignored)
        .transitional_ignore();
    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(authzSession->lookupUser(UserName("spencer", "test")));
}

TEST_F(AuthorizationSessionTest, UseOldUserInfoInFaceOfConnectivityProblems) {
    // Add a readWrite user
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(UserName("spencer", "test"));
    ASSERT(user->isValid());

    // Change the user to be read-only
    int ignored;
    managerState->setFindsShouldFail(true);
    managerState
        ->remove(_opCtx.get(),
                 AuthorizationManager::usersCollectionNamespace,
                 BSONObj(),
                 BSONObj(),
                 &ignored)
        .transitional_ignore();
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Even though the user's privileges have been reduced, since we've configured user
    // document lookup to fail, the authz session should continue to use its known out-of-date
    // privilege data.
    authzManager->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    // Once we configure document lookup to succeed again, authorization checks should
    // observe the new values.
    managerState->setFindsShouldFail(false);
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
}

TEST_F(AuthorizationSessionTest, AcquireUserFailsWithOldFeatureCompatibilityVersion) {
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "readWrite"
                                                                            << "db"
                                                                            << "test"))
                                                         << "authenticationRestrictions"
                                                         << BSON_ARRAY(BSON(
                                                                "clientSource"
                                                                << BSON_ARRAY("192.168.0.0/24"
                                                                              << "192.168.2.10")
                                                                << "serverAddress"
                                                                << BSON_ARRAY("192.168.0.2")))),
                                                    BSONObj()));

    serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k34);

    RestrictionEnvironment::set(
        session,
        stdx::make_unique<RestrictionEnvironment>(SockAddr("192.168.0.6", 5555, AF_UNSPEC),
                                                  SockAddr("192.168.0.2", 5555, AF_UNSPEC)));

    ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
}

TEST_F(AuthorizationSessionTest, RefreshRemovesRestrictedUsersDuringFeatureCompatibilityDowngrade) {
    ASSERT_OK(managerState->insertPrivilegeDocument(
        _opCtx.get(),
        BSON("user"
             << "spencer"
             << "db"
             << "test"
             << "credentials"
             << BSON("MONGODB-CR"
                     << "a")
             << "roles"
             << BSON_ARRAY(BSON("role"
                                << "readWrite"
                                << "db"
                                << "test"))
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("192.168.0.0/24") << "serverAddress"
                                               << BSON_ARRAY("192.168.0.2")))),
        BSONObj()));

    RestrictionEnvironment::set(
        session,
        stdx::make_unique<RestrictionEnvironment>(SockAddr("192.168.0.6", 5555, AF_UNSPEC),
                                                  SockAddr("192.168.0.2", 5555, AF_UNSPEC)));

    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k34);

    ASSERT_TRUE(authzSession->lookupUser(UserName("spencer", "test")));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));

    authzManager->invalidateUserCache();
    authzSession->startRequest(_opCtx.get());

    ASSERT_FALSE(authzSession->lookupUser(UserName("spencer", "test")));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
}

TEST_F(AuthorizationSessionTest, AcquireUserObtainsAndValidatesAuthenticationRestrictions) {
    ASSERT_OK(managerState->insertPrivilegeDocument(
        _opCtx.get(),
        BSON("user"
             << "spencer"
             << "db"
             << "test"
             << "credentials"
             << BSON("MONGODB-CR"
                     << "a")
             << "roles"
             << BSON_ARRAY(BSON("role"
                                << "readWrite"
                                << "db"
                                << "test"))
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("192.168.0.0/24"
                                                             << "192.168.2.10")
                                               << "serverAddress"
                                               << BSON_ARRAY("192.168.0.2"))
                           << BSON("clientSource" << BSON_ARRAY("2001:DB8::1") << "serverAddress"
                                                  << BSON_ARRAY("2001:DB8::2"))
                           << BSON("clientSource" << BSON_ARRAY("127.0.0.1"
                                                                << "::1")
                                                  << "serverAddress"
                                                  << BSON_ARRAY("127.0.0.1"
                                                                << "::1")))),
        BSONObj()));


    auto assertWorks = [this](StringData clientSource, StringData serverAddress) {
        RestrictionEnvironment::set(
            session,
            stdx::make_unique<RestrictionEnvironment>(SockAddr(clientSource, 5555, AF_UNSPEC),
                                                      SockAddr(serverAddress, 27017, AF_UNSPEC)));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    };

    auto assertFails = [this](StringData clientSource, StringData serverAddress) {
        RestrictionEnvironment::set(
            session,
            stdx::make_unique<RestrictionEnvironment>(SockAddr(clientSource, 5555, AF_UNSPEC),
                                                      SockAddr(serverAddress, 27017, AF_UNSPEC)));
        ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    };

    // The empty RestrictionEnvironment will cause addAndAuthorizeUser to fail.
    ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    // A clientSource from the 192.168.0.0/24 block will succeed in connecting to a server
    // listening on 192.168.0.2.
    assertWorks("192.168.0.6", "192.168.0.2");
    assertWorks("192.168.0.12", "192.168.0.2");

    // A client connecting from the explicitly whitelisted addresses can connect to a
    // server listening on 192.168.0.2
    assertWorks("192.168.2.10", "192.168.0.2");

    // A client from either of these sources must connect to the server via the serverAddress
    // expressed in the restriction.
    assertFails("192.168.0.12", "127.0.0.1");
    assertFails("192.168.2.10", "127.0.0.1");
    assertFails("192.168.0.12", "192.168.1.3");
    assertFails("192.168.2.10", "192.168.1.3");

    // A client outside of these two sources cannot connect to the server.
    assertFails("192.168.1.12", "192.168.0.2");
    assertFails("192.168.1.10", "192.168.0.2");


    // An IPv6 client from the correct address may use the IPv6 restriction to connect to the
    // server.
    assertWorks("2001:DB8::1", "2001:DB8::2");
    assertFails("2001:DB8::1", "2001:DB8::3");
    assertFails("2001:DB8::2", "2001:DB8::1");

    // A localhost client can connect to a localhost server, using the second addressRestriction
    assertWorks("127.0.0.1", "127.0.0.1");
    assertWorks("::1", "::1");
    assertWorks("::1", "127.0.0.1");  // Silly case
    assertWorks("127.0.0.1", "::1");  // Silly case
    assertFails("192.168.0.6", "127.0.0.1");
    assertFails("127.0.0.1", "192.168.0.2");
}

TEST_F(AuthorizationSessionTest, CheckAuthForAggregateFailsIfPipelineIsNotAnArray) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONObj cmdObjIntPipeline = BSON("aggregate" << testFooNss.coll() << "pipeline" << 7);
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              authzSession->checkAuthForAggregate(testFooNss, cmdObjIntPipeline, false));

    BSONObj cmdObjObjPipeline = BSON("aggregate" << testFooNss.coll() << "pipeline" << BSONObj());
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              authzSession->checkAuthForAggregate(testFooNss, cmdObjObjPipeline, false));

    BSONObj cmdObjNoPipeline = BSON("aggregate" << testFooNss.coll());
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              authzSession->checkAuthForAggregate(testFooNss, cmdObjNoPipeline, false));
}

TEST_F(AuthorizationSessionTest, CheckAuthForAggregateFailsIfPipelineFirstStageIsNotAnObject) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONObj cmdObjFirstStageInt =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << BSON_ARRAY(7));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              authzSession->checkAuthForAggregate(testFooNss, cmdObjFirstStageInt, false));

    BSONObj cmdObjFirstStageArray =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << BSON_ARRAY(BSONArray()));
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              authzSession->checkAuthForAggregate(testFooNss, cmdObjFirstStageArray, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateEmptyPipelineWithoutFindAction) {
    BSONObj cmdObj = BSON("aggregate" << testFooNss.coll() << "pipeline" << BSONArray() << "cursor"
                                      << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateEmptyPipelineWithFindAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONObj cmdObj = BSON("aggregate" << testFooNss.coll() << "pipeline" << BSONArray() << "cursor"
                                      << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithoutFindActionIfFirstStageNotIndexOrCollStats) {
    authzSession->assumePrivilegesForDB(
        Privilege(testFooCollResource, {ActionType::indexStats, ActionType::collStats}));

    BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                        << BSON("$indexStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithFindActionIfPipelineContainsIndexOrCollStats) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));
    BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                        << BSON("$indexStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCollStatsWithoutCollStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateCollStatsWithCollStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::collStats}));

    BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateIndexStatsWithoutIndexStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateIndexStatsWithIndexStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::indexStats}));

    BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, true));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoD) {
    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());

    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoS) {
    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());

    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, true));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, true));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(), {ActionType::inprog}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(), {ActionType::inprog}));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, true));
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline =
        BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline =
        BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, true));
}

TEST_F(AuthorizationSessionTest, AddPrivilegesForStageFailsIfOutNamespaceIsNotValid) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$out"
                                         << ""));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_THROWS_CODE(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false).ignore(),
                       UserException,
                       ErrorCodes::InvalidNamespace);
}

TEST_F(AuthorizationSessionTest, CannotAggregateOutWithoutInsertAndRemoveOnTargetNamespace) {
    // We only have find on the aggregation namespace.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));

    // We have insert but not remove on the $out namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::insert})});
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));

    // We have remove but not insert on the $out namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::remove})});
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateOutWithInsertAndRemoveOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, {ActionType::find}),
         Privilege(testBarCollResource, {ActionType::insert, ActionType::remove})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));

    BSONObj cmdObjNoBypassDocumentValidation = BSON(
        "aggregate" << testFooNss.coll() << "pipeline" << pipeline << "bypassDocumentValidation"
                    << false
                    << "cursor"
                    << BSONObj());
    ASSERT_OK(
        authzSession->checkAuthForAggregate(testFooNss, cmdObjNoBypassDocumentValidation, false));
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateOutBypassingValidationWithoutBypassDocumentValidationOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, {ActionType::find}),
         Privilege(testBarCollResource, {ActionType::insert, ActionType::remove})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "bypassDocumentValidation"
                         << true);
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest,
       CanAggregateOutBypassingValidationWithBypassDocumentValidationOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, {ActionType::find}),
         Privilege(
             testBarCollResource,
             {ActionType::insert, ActionType::remove, ActionType::bypassDocumentValidation})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "bypassDocumentValidation"
                         << true);
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testBarNss.coll())));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find})});

    BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testBarNss.coll())));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}


TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnNestedJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find})});

    BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testQuxNss.coll())));
    BSONArray pipeline = BSON_ARRAY(
        BSON("$lookup" << BSON("from" << testBarNss.coll() << "pipeline" << nestedPipeline)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnNestedJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find}),
                                         Privilege(testQuxCollResource, {ActionType::find})});

    BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testQuxNss.coll())));
    BSONArray pipeline = BSON_ARRAY(
        BSON("$lookup" << BSON("from" << testBarNss.coll() << "pipeline" << nestedPipeline)));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CheckAuthForAggregateWithDeeplyNestedLookup) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    // Recursively adds nested $lookup stages to 'pipelineBob', building a pipeline with
    // 'levelsToGo' deep $lookup stages.
    stdx::function<void(BSONArrayBuilder*, int)> addNestedPipeline;
    addNestedPipeline = [&addNestedPipeline](BSONArrayBuilder* pipelineBob, int levelsToGo) {
        if (levelsToGo == 0) {
            return;
        }

        BSONObjBuilder objectBob(pipelineBob->subobjStart());
        BSONObjBuilder lookupBob(objectBob.subobjStart("$lookup"));
        lookupBob << "from" << testFooNss.coll() << "as"
                  << "as";
        BSONArrayBuilder subPipelineBob(lookupBob.subarrayStart("pipeline"));
        addNestedPipeline(&subPipelineBob, --levelsToGo);
        subPipelineBob.doneFast();
        lookupBob.doneFast();
        objectBob.doneFast();
    };

    // checkAuthForAggregate() should succeed for an aggregate command that has a deeply nested
    // $lookup sub-pipeline chain. Each nested $lookup stage adds 3 to the depth of the command
    // object. We set 'maxLookupDepth' depth to allow for a command object that is at or just under
    // max BSONDepth.
    const uint32_t aggregateCommandDepth = 1;
    const uint32_t lookupDepth = 3;
    const uint32_t maxLookupDepth =
        (BSONDepth::getMaxAllowableDepth() - aggregateCommandDepth) / lookupDepth;

    BSONObjBuilder cmdBuilder;
    cmdBuilder << "aggregate" << testFooNss.coll();
    BSONArrayBuilder pipelineBuilder(cmdBuilder.subarrayStart("pipeline"));
    addNestedPipeline(&pipelineBuilder, maxLookupDepth);
    pipelineBuilder.doneFast();
    cmdBuilder << "cursor" << BSONObj();

    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdBuilder.obj(), false));
}


TEST_F(AuthorizationSessionTest, CannotAggregateGraphLookupWithoutFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << testBarNss.coll())));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, CanAggregateGraphLookupWithFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find})});

    BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << testBarNss.coll())));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateFacetWithLookupAndGraphLookupWithoutFindOnJoinedNamespaces) {
    // We only have find on the aggregation namespace.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, {ActionType::find}));

    BSONArray pipeline =
        BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                            "[{$graphLookup: {from: 'qux'}}]}}"));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));

    // We have find on the $lookup namespace but not on the $graphLookup namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find})});
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));

    // We have find on the $graphLookup namespace but not on the $lookup namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testQuxCollResource, {ActionType::find})});
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest,
       CanAggregateFacetWithLookupAndGraphLookupWithFindOnJoinedNamespaces) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, {ActionType::find}),
                                         Privilege(testBarCollResource, {ActionType::find}),
                                         Privilege(testQuxCollResource, {ActionType::find})});

    BSONArray pipeline =
        BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                            "[{$graphLookup: {from: 'qux'}}]}}"));
    BSONObj cmdObj =
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj());
    ASSERT_OK(authzSession->checkAuthForAggregate(testFooNss, cmdObj, false));
}

TEST_F(AuthorizationSessionTest, UnauthorizedSessionIsCoauthorizedWithEmptyUserSet) {
    std::vector<UserName> userSet;
    ASSERT_TRUE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest, UnauthorizedSessionIsNotCoauthorizedWithNonemptyUserSet) {
    std::vector<UserName> userSet;
    userSet.emplace_back("spencer", "test");
    ASSERT_FALSE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest,
       UnauthorizedSessionIsCoauthorizedWithNonemptyUserSetWhenAuthIsDisabled) {
    authzManager->setAuthEnabled(false);
    std::vector<UserName> userSet;
    userSet.emplace_back("spencer", "test");
    ASSERT_TRUE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest, AuthorizedSessionIsNotCoauthorizedWithEmptyUserSet) {
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    std::vector<UserName> userSet;
    ASSERT_FALSE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest,
       AuthorizedSessionIsCoauthorizedWithEmptyUserSetWhenAuthIsDisabled) {
    authzManager->setAuthEnabled(false);
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    std::vector<UserName> userSet;
    ASSERT_TRUE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest, AuthorizedSessionIsCoauthorizedWithIntersectingUserSet) {
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("admin", "test")));
    std::vector<UserName> userSet;
    userSet.emplace_back("admin", "test");
    userSet.emplace_back("tess", "test");
    ASSERT_TRUE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest, AuthorizedSessionIsNotCoauthorizedWithNonintersectingUserSet) {
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("admin", "test")));
    std::vector<UserName> userSet;
    userSet.emplace_back("tess", "test");
    ASSERT_FALSE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest,
       AuthorizedSessionIsCoauthorizedWithNonintersectingUserSetWhenAuthIsDisabled) {
    authzManager->setAuthEnabled(false);
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials"
                                                         << BSON("MONGODB-CR"
                                                                 << "a")
                                                         << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("admin", "test")));
    std::vector<UserName> userSet;
    userSet.emplace_back("tess", "test");
    ASSERT_TRUE(
        authzSession->isCoauthorizedWith(makeUserNameIterator(userSet.begin(), userSet.end())));
}

TEST_F(AuthorizationSessionTest, CannotListCollectionsWithoutListCollectionsPrivilege) {
    // With no privileges, there is not authorization to list collections
    ASSERT_FALSE(authzSession->isAuthorizedToListCollections(testFooNss.db()));
    ASSERT_FALSE(authzSession->isAuthorizedToListCollections(testBarNss.db()));
    ASSERT_FALSE(authzSession->isAuthorizedToListCollections(testQuxNss.db()));
}

TEST_F(AuthorizationSessionTest, CanListCollectionsWithLegacySystemNamespacesAccess) {
    // Deprecated: permissions for the find action on test.system.namespaces allows us to list
    // collections in the test database.
    authzSession->assumePrivilegesForDB(
        Privilege(testSystemNamespacesResource, {ActionType::find}));

    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testFooNss.db()));
    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testBarNss.db()));
    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testQuxNss.db()));
}

TEST_F(AuthorizationSessionTest, CanListCollectionsWithListCollectionsPrivilege) {
    // The listCollections privilege authorizes the list collections command.
    authzSession->assumePrivilegesForDB(Privilege(testDBResource, {ActionType::listCollections}));

    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testFooNss.db()));
    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testBarNss.db()));
    ASSERT_TRUE(authzSession->isAuthorizedToListCollections(testQuxNss.db()));
}

}  // namespace
}  // namespace mongo
