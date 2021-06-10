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
#include "mongo/bson/bson_depth.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class FailureCapableAuthzManagerExternalStateMock : public AuthzManagerExternalStateMock {
public:
    FailureCapableAuthzManagerExternalStateMock() = default;
    ~FailureCapableAuthzManagerExternalStateMock() = default;

    void setFindsShouldFail(bool enable) {
        _findsShouldFail = enable;
    }

    Status findOne(OperationContext* opCtx,
                   const NamespaceString& collectionName,
                   const BSONObj& query,
                   BSONObj* result) override {
        if (_findsShouldFail && collectionName == AuthorizationManager::usersCollectionNamespace) {
            return Status(ErrorCodes::UnknownError,
                          "findOne on admin.system.users set to fail in mock.");
        }
        return AuthzManagerExternalStateMock::findOne(opCtx, collectionName, query, result);
    }

private:
    bool _findsShouldFail{false};
};

class AuthorizationSessionTest : public ScopedGlobalServiceContextForTest, public unittest::Test {
public:
    void setUp() {
        _session = transportLayer.createSession();
        _client = getServiceContext()->makeClient("testClient", _session);
        RestrictionEnvironment::set(
            _session, std::make_unique<RestrictionEnvironment>(SockAddr(), SockAddr()));
        _opCtx = _client->makeOperationContext();
        auto localManagerState = std::make_unique<FailureCapableAuthzManagerExternalStateMock>();
        managerState = localManagerState.get();
        managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        auto uniqueAuthzManager = std::make_unique<AuthorizationManagerImpl>(
            getServiceContext(), std::move(localManagerState));
        authzManager = uniqueAuthzManager.get();
        AuthorizationManager::set(getServiceContext(), std::move(uniqueAuthzManager));
        auto localSessionState = std::make_unique<AuthzSessionExternalStateMock>(authzManager);
        sessionState = localSessionState.get();
        authzSession = std::make_unique<AuthorizationSessionForTest>(
            std::move(localSessionState),
            AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
        authzManager->setAuthEnabled(true);

        credentials =
            BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                      "a", saslGlobalParams.scramSHA1IterationCount.load())
                               << "SCRAM-SHA-256"
                               << scram::Secrets<SHA256Block>::generateCredentials(
                                      "a", saslGlobalParams.scramSHA256IterationCount.load()));
    }

    void tearDown() override {
        authzSession->logoutAllDatabases(_client.get(), "Ending AuthorizationSessionTest");
    }

protected:
    FailureCapableAuthzManagerExternalStateMock* managerState;
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle _session;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    AuthzSessionExternalStateMock* sessionState;
    AuthorizationManager* authzManager;
    std::unique_ptr<AuthorizationSessionForTest> authzSession;
    BSONObj credentials;
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
const ResourcePattern testProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("test.system.profile")));
const ResourcePattern otherProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("other.system.profile")));
const ResourcePattern thirdProfileCollResource(
    ResourcePattern::forExactNamespace(NamespaceString("third.system.profile")));

TEST_F(AuthorizationSessionTest, AddUserAndCheckAuthorization) {
    authzSession->startContractTracking();

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
                                                         << "credentials" << credentials << "roles"
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
                                                   << "credentials" << credentials << "roles"
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

    authzSession->logoutDatabase(_client.get(), "test", "Kill the test!");
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));

    authzSession->logoutDatabase(_client.get(), "admin", "Fire the admin!");
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));

    // Verify we recorded the all the auth checks correctly
    AuthorizationContract ac(
        std::initializer_list<AccessCheckEnum>{},
        std::initializer_list<Privilege>{
            Privilege(ResourcePattern::forDatabaseName("ignored"),
                      {ActionType::insert, ActionType::dbStats}),
            Privilege(ResourcePattern::forExactNamespace(NamespaceString("ignored.ignored")),
                      {ActionType::insert, ActionType::collMod}),
        });

    authzSession->verifyContract(&ac);

    // Verify against a smaller contract that verifyContract fails
    AuthorizationContract acMissing(std::initializer_list<AccessCheckEnum>{},
                                    std::initializer_list<Privilege>{
                                        Privilege(ResourcePattern::forDatabaseName("ignored"),
                                                  {ActionType::insert, ActionType::dbStats}),
                                    });
    ASSERT_THROWS_CODE(authzSession->verifyContract(&acMissing), AssertionException, 5452401);
}

TEST_F(AuthorizationSessionTest, DuplicateRolesOK) {
    // Add a user with doubled-up readWrite and single dbAdmin on the test DB
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
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
                                                   << "credentials" << credentials << "roles"
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
                                                   << "credentials" << credentials << "roles"
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
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
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
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
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
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
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
        authzSession->isAuthorizedForActionsOnResource(testProfileCollResource, ActionType::find));
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
                                                         << "credentials" << credentials << "roles"
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

    // Change the user to be read-only
    int ignored;
    ASSERT_OK(managerState->remove(_opCtx.get(),
                                   AuthorizationManager::usersCollectionNamespace,
                                   BSONObj(),
                                   BSONObj(),
                                   &ignored));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(_opCtx.get(), user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    user = authzSession->lookupUser(UserName("spencer", "test"));

    // Delete the user.
    ASSERT_OK(managerState->remove(_opCtx.get(),
                                   AuthorizationManager::usersCollectionNamespace,
                                   BSONObj(),
                                   BSONObj(),
                                   &ignored));
    // Make sure that invalidating the user causes the session to reload its privileges.
    authzManager->invalidateUserByName(_opCtx.get(), user->getName());
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
                                                         << "credentials" << credentials << "roles"
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

    // Change the user to be read-only
    int ignored;
    managerState->setFindsShouldFail(true);
    ASSERT_OK(managerState->remove(_opCtx.get(),
                                   AuthorizationManager::usersCollectionNamespace,
                                   BSONObj(),
                                   BSONObj(),
                                   &ignored));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "spencer"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
                                                         << BSON_ARRAY(BSON("role"
                                                                            << "read"
                                                                            << "db"
                                                                            << "test"))),
                                                    BSONObj()));

    // Even though the user's privileges have been reduced, since we've configured user
    // document lookup to fail, the authz session should continue to use its known out-of-date
    // privilege data.
    authzManager->invalidateUserByName(_opCtx.get(), user->getName());
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

TEST_F(AuthorizationSessionTest, AcquireUserObtainsAndValidatesAuthenticationRestrictions) {
    ASSERT_OK(managerState->insertPrivilegeDocument(
        _opCtx.get(),
        BSON("user"
             << "spencer"
             << "db"
             << "test"
             << "credentials" << credentials << "roles"
             << BSON_ARRAY(BSON("role"
                                << "readWrite"
                                << "db"
                                << "test"))
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("192.168.0.0/24"
                                                             << "192.168.2.10")
                                               << "serverAddress" << BSON_ARRAY("192.168.0.2"))
                           << BSON("clientSource" << BSON_ARRAY("2001:DB8::1") << "serverAddress"
                                                  << BSON_ARRAY("2001:DB8::2"))
                           << BSON("clientSource" << BSON_ARRAY("127.0.0.1"
                                                                << "::1")
                                                  << "serverAddress"
                                                  << BSON_ARRAY("127.0.0.1"
                                                                << "::1")))),
        BSONObj()));


    auto assertWorks = [this](StringData clientSource, StringData serverAddress) {
        RestrictionEnvironment::set(_session,
                                    std::make_unique<RestrictionEnvironment>(
                                        SockAddr::create(clientSource, 5555, AF_UNSPEC),
                                        SockAddr::create(serverAddress, 27017, AF_UNSPEC)));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    };

    auto assertFails = [this](StringData clientSource, StringData serverAddress) {
        RestrictionEnvironment::set(_session,
                                    std::make_unique<RestrictionEnvironment>(
                                        SockAddr::create(clientSource, 5555, AF_UNSPEC),
                                        SockAddr::create(serverAddress, 27017, AF_UNSPEC)));
        ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));
    };

    // The empty RestrictionEnvironment will cause addAndAuthorizeUser to fail.
    ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), UserName("spencer", "test")));

    // A clientSource from the 192.168.0.0/24 block will succeed in connecting to a server
    // listening on 192.168.0.2.
    assertWorks("192.168.0.6", "192.168.0.2");
    assertWorks("192.168.0.12", "192.168.0.2");

    // A client connecting from the explicitly allowlisted addresses can connect to a
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

TEST_F(AuthorizationSessionTest, CannotAggregateEmptyPipelineWithoutFindAction) {
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << BSONArray() << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateEmptyPipelineWithFindAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << BSONArray() << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithoutFindActionIfFirstStageNotIndexOrCollStats) {
    authzSession->assumePrivilegesForDB(
        Privilege(testFooCollResource, {ActionType::indexStats, ActionType::collStats}));

    BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                        << BSON("$indexStats" << BSONObj()));

    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithFindActionIfPipelineContainsIndexOrCollStats) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));
    BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                        << BSON("$indexStats" << BSONObj()));

    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCollStatsWithoutCollStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateCollStatsWithCollStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::collStats));

    BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateIndexStatsWithoutIndexStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateIndexStatsWithIndexStatsAction) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::indexStats));

    BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoD) {
    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    ASSERT_FALSE(authzSession->isAuthenticated());
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoS) {
    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));

    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(), ActionType::inprog));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(), ActionType::inprog));

    BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoD) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline =
        BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoS) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline =
        BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, AddPrivilegesForStageFailsIfOutNamespaceIsNotValid) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$out"
                                         << ""));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    ASSERT_THROWS_CODE(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(AuthorizationSessionTest, CannotAggregateOutWithoutInsertAndRemoveOnTargetNamespace) {
    // We only have find on the aggregation namespace.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

    // We have insert but not remove on the $out namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::insert)});
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

    // We have remove but not insert on the $out namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::remove)});
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateOutWithInsertAndRemoveOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, ActionType::find),
         Privilege(testBarCollResource, {ActionType::insert, ActionType::remove})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));

    auto aggNoBypassDocumentValidationReq =
        uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
            testFooNss,
            BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline
                             << "bypassDocumentValidation" << false << "cursor" << BSONObj()
                             << "$db" << testFooNss.db())));

    privileges = uassertStatusOK(auth::getPrivilegesForAggregate(
        authzSession.get(), testFooNss, aggNoBypassDocumentValidationReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateOutBypassingValidationWithoutBypassDocumentValidationOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, ActionType::find),
         Privilege(testBarCollResource, {ActionType::insert, ActionType::remove})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "bypassDocumentValidation" << true << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest,
       CanAggregateOutBypassingValidationWithBypassDocumentValidationOnTargetNamespace) {
    authzSession->assumePrivilegesForDB(
        {Privilege(testFooCollResource, ActionType::find),
         Privilege(
             testBarCollResource,
             {ActionType::insert, ActionType::remove, ActionType::bypassDocumentValidation})});

    BSONArray pipeline = BSON_ARRAY(BSON("$out" << testBarNss.coll()));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "bypassDocumentValidation" << true << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testBarNss.coll())));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find)});

    BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testBarNss.coll())));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}


TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnNestedJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find)});

    BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testQuxNss.coll())));
    BSONArray pipeline = BSON_ARRAY(
        BSON("$lookup" << BSON("from" << testBarNss.coll() << "pipeline" << nestedPipeline)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnNestedJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find),
                                         Privilege(testQuxCollResource, ActionType::find)});

    BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << testQuxNss.coll())));
    BSONArray pipeline = BSON_ARRAY(
        BSON("$lookup" << BSON("from" << testBarNss.coll() << "pipeline" << nestedPipeline)));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CheckAuthForAggregateWithDeeplyNestedLookup) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    // Recursively adds nested $lookup stages to 'pipelineBob', building a pipeline with
    // 'levelsToGo' deep $lookup stages.
    std::function<void(BSONArrayBuilder*, int)> addNestedPipeline;
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
    cmdBuilder << "cursor" << BSONObj() << "$db" << testFooNss.db();

    auto aggReq = uassertStatusOK(
        aggregation_request_helper::parseFromBSONForTests(testFooNss, cmdBuilder.obj()));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}


TEST_F(AuthorizationSessionTest, CannotAggregateGraphLookupWithoutFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << testBarNss.coll())));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest, CanAggregateGraphLookupWithFindOnJoinedNamespace) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find)});

    BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << testBarNss.coll())));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateFacetWithLookupAndGraphLookupWithoutFindOnJoinedNamespaces) {
    // We only have find on the aggregation namespace.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    BSONArray pipeline =
        BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                            "[{$graphLookup: {from: 'qux'}}]}}"));
    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

    // We have find on the $lookup namespace but not on the $graphLookup namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find)});
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

    // We have find on the $graphLookup namespace but not on the $lookup namespace.
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testQuxCollResource, ActionType::find)});
    ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
}

TEST_F(AuthorizationSessionTest,
       CanAggregateFacetWithLookupAndGraphLookupWithFindOnJoinedNamespaces) {
    authzSession->assumePrivilegesForDB({Privilege(testFooCollResource, ActionType::find),
                                         Privilege(testBarCollResource, ActionType::find),
                                         Privilege(testQuxCollResource, ActionType::find)});

    BSONArray pipeline =
        BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                            "[{$graphLookup: {from: 'qux'}}]}}"));

    auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        testFooNss,
        BSON("aggregate" << testFooNss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "$db" << testFooNss.db())));
    PrivilegeVector privileges = uassertStatusOK(
        auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, true));
    ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
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
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
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
                                                         << "credentials" << credentials << "roles"
                                                         << BSONArray()),
                                                    BSONObj()));
    ASSERT_OK(managerState->insertPrivilegeDocument(_opCtx.get(),
                                                    BSON("user"
                                                         << "admin"
                                                         << "db"
                                                         << "test"
                                                         << "credentials" << credentials << "roles"
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
    BSONObj cmd = BSON("listCollections" << 1);
    // With no privileges, there is not authorization to list collections
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(testFooNss.db(), cmd).getStatus());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(testBarNss.db(), cmd).getStatus());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(testQuxNss.db(), cmd).getStatus());
}

TEST_F(AuthorizationSessionTest, CanListCollectionsWithListCollectionsPrivilege) {
    BSONObj cmd = BSON("listCollections" << 1);
    // The listCollections privilege authorizes the list collections command.
    authzSession->assumePrivilegesForDB(Privilege(testDBResource, ActionType::listCollections));

    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testFooNss.db(), cmd).getStatus());
    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testBarNss.db(), cmd).getStatus());
    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testQuxNss.db(), cmd).getStatus());
}

TEST_F(AuthorizationSessionTest, CanListOwnCollectionsWithPrivilege) {
    BSONObj cmd =
        BSON("listCollections" << 1 << "nameOnly" << true << "authorizedCollections" << true);
    // The listCollections privilege authorizes the list collections command.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));

    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testFooNss.db(), cmd).getStatus());
    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testBarNss.db(), cmd).getStatus());
    ASSERT_OK(authzSession->checkAuthorizedToListCollections(testQuxNss.db(), cmd).getStatus());

    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections("other", cmd).getStatus());
}

TEST_F(AuthorizationSessionTest, CanCheckIfHasAnyPrivilegeOnResource) {
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));

    // If we have a collection privilege, we have actions on that collection
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(testFooNss.db())));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));

    // If we have a database privilege, we have actions on that database and all collections it
    // contains
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forDatabaseName(testFooNss.db()), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(testFooNss.db())));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));

    // If we have a privilege on anyNormalResource, we have actions on all databases and all
    // collections they contain
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnyNormalResource(), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(testFooNss.db())));
    ASSERT_TRUE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));
}

TEST_F(AuthorizationSessionTest, CanUseUUIDNamespacesWithPrivilege) {
    BSONObj stringObj = BSON("a"
                             << "string");
    BSONObj uuidObj = BSON("a" << UUID::gen());
    BSONObj invalidObj = BSON("a" << 12);

    authzSession->startContractTracking();

    // Strings require no privileges
    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(stringObj.firstElement()));

    // UUIDs cannot be parsed with default privileges
    ASSERT_FALSE(authzSession->isAuthorizedToParseNamespaceElement(uuidObj.firstElement()));

    // Element must be either a string, or a UUID
    ASSERT_THROWS_CODE(authzSession->isAuthorizedToParseNamespaceElement(invalidObj.firstElement()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);

    // The useUUID privilege allows UUIDs to be parsed
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(), ActionType::useUUID));

    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(stringObj.firstElement()));
    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(uuidObj.firstElement()));
    ASSERT_THROWS_CODE(authzSession->isAuthorizedToParseNamespaceElement(invalidObj.firstElement()),
                       AssertionException,
                       ErrorCodes::InvalidNamespace);

    // Verify we recorded the all the auth checks correctly
    AuthorizationContract ac(
        std::initializer_list<AccessCheckEnum>{
            AccessCheckEnum::kIsAuthorizedToParseNamespaceElement},
        std::initializer_list<Privilege>{
            Privilege(ResourcePattern::forClusterResource(), ActionType::useUUID)});

    authzSession->verifyContract(&ac);
}

class SystemBucketsTest : public AuthorizationSessionTest {
protected:
    static constexpr auto sb_db_test = "sb_db_test"_sd;
    static constexpr auto sb_db_other = "sb_db_other"_sd;
    static constexpr auto sb_coll_test = "sb_coll_test"_sd;

    static const ResourcePattern testMissingSystemBucketResource;
    static const ResourcePattern otherMissingSystemBucketResource;
    static const ResourcePattern otherDbMissingSystemBucketResource;

    static const ResourcePattern testSystemBucketResource;
    static const ResourcePattern otherSystemBucketResource;
    static const ResourcePattern otherDbSystemBucketResource;

    static const ResourcePattern testBucketResource;
    static const ResourcePattern otherBucketResource;
    static const ResourcePattern otherDbBucketResource;
};

const ResourcePattern SystemBucketsTest::testMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_test.sb_coll_test")));
const ResourcePattern SystemBucketsTest::otherMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_test.sb_coll_other")));
const ResourcePattern SystemBucketsTest::otherDbMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_other.sb_coll_test")));

const ResourcePattern SystemBucketsTest::testSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_test.system.buckets.sb_coll_test")));
const ResourcePattern SystemBucketsTest::otherSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_test.system.buckets.sb_coll_other")));
const ResourcePattern SystemBucketsTest::otherDbSystemBucketResource(
    ResourcePattern::forExactNamespace(NamespaceString("sb_db_other.system.buckets.sb_coll_test")));

const ResourcePattern SystemBucketsTest::testBucketResource(
    ResourcePattern::forExactSystemBucketsCollection("sb_db_test", "sb_coll_test"));
const ResourcePattern SystemBucketsTest::otherBucketResource(
    ResourcePattern::forExactSystemBucketsCollection("sb_db_test", "sb_coll_other"));
const ResourcePattern SystemBucketsTest::otherDbBucketResource(
    ResourcePattern::forExactSystemBucketsCollection("sb_db_other", "sb_coll_test"));

TEST_F(SystemBucketsTest, CheckExactSystemBucketsCollection) {
    // If we have a system_buckets exact priv
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource,
                                                                ActionType::insert));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                                ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                                ActionType::find));


    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testMissingSystemBucketResource,
                                                                ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherMissingSystemBucketResource,
                                                                ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbMissingSystemBucketResource,
                                                                ActionType::find));
}

TEST_F(SystemBucketsTest, CheckAnySystemBuckets) {
    // If we have an any system_buckets priv
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBuckets(), ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource,
                                                                ActionType::insert));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                               ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                               ActionType::find));


    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testMissingSystemBucketResource,
                                                                ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherMissingSystemBucketResource,
                                                                ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbMissingSystemBucketResource,
                                                                ActionType::find));
}

TEST_F(SystemBucketsTest, CheckAnySystemBucketsInDatabase) {
    // If we have a system_buckets in a db priv
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase("sb_db_test"), ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource,
                                                                ActionType::insert));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                               ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                                ActionType::find));


    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testMissingSystemBucketResource,
                                                                ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherMissingSystemBucketResource,
                                                                ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbMissingSystemBucketResource,
                                                                ActionType::find));
}

TEST_F(SystemBucketsTest, CheckforAnySystemBucketsInAnyDatabase) {
    // If we have a system_buckets for a coll in any db priv
    authzSession->assumePrivilegesForDB(Privilege(
        ResourcePattern::forAnySystemBucketsInAnyDatabase("sb_coll_test"), ActionType::find));


    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource,
                                                                ActionType::insert));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                                ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                               ActionType::find));


    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(testMissingSystemBucketResource,
                                                                ActionType::find));
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherMissingSystemBucketResource,
                                                                ActionType::find));

    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(otherDbMissingSystemBucketResource,
                                                                ActionType::find));
}

TEST_F(SystemBucketsTest, CanCheckIfHasAnyPrivilegeOnResourceForSystemBuckets) {
    // If we have a system.buckets collection privilege, we have actions on that collection
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));

    // If we have any buckets in a database privilege, we have actions on that database and all
    // system.buckets collections it contains
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test)));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));

    // If we have a privilege on any systems buckets in any db, we have actions on all databases and
    // system.buckets.<coll> they contain
    authzSession->assumePrivilegesForDB(Privilege(
        ResourcePattern::forAnySystemBucketsInAnyDatabase(sb_coll_test), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyNormalResource()));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forAnyResource()));
}

TEST_F(SystemBucketsTest, CheckBuiltinRolesForSystemBuckets) {
    // If we have readAnyDatabase, make sure we can read system.buckets anywhere
    authzSession->assumePrivilegesForBuiltinRole(RoleName("readAnyDatabase", "admin"));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                               ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                               ActionType::find));

    // If we have readAnyDatabase, make sure we can read and write system.buckets anywhere
    authzSession->assumePrivilegesForBuiltinRole(RoleName("readWriteAnyDatabase", "admin"));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        testSystemBucketResource, {ActionType::find, ActionType::insert}));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherSystemBucketResource, {ActionType::find, ActionType::insert}));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherDbSystemBucketResource, {ActionType::find, ActionType::insert}));

    // If we have readAnyDatabase, make sure we can do admin stuff on system.buckets anywhere
    authzSession->assumePrivilegesForBuiltinRole(RoleName("dbAdminAnyDatabase", "admin"));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        testSystemBucketResource, ActionType::bypassDocumentValidation));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherSystemBucketResource, ActionType::bypassDocumentValidation));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherDbSystemBucketResource, ActionType::bypassDocumentValidation));


    // If we have readAnyDatabase, make sure we can do restore stuff on system.buckets anywhere
    authzSession->assumePrivilegesForBuiltinRole(RoleName("restore", "admin"));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        testSystemBucketResource, ActionType::bypassDocumentValidation));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherSystemBucketResource, ActionType::bypassDocumentValidation));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        otherDbSystemBucketResource, ActionType::bypassDocumentValidation));

    // If we have readAnyDatabase, make sure we can do restore stuff on system.buckets anywhere
    authzSession->assumePrivilegesForBuiltinRole(RoleName("backup", "admin"));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testSystemBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherSystemBucketResource,
                                                               ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(otherDbSystemBucketResource,
                                                               ActionType::find));
}

TEST_F(SystemBucketsTest, CanCheckIfHasAnyPrivilegeInResourceDBForSystemBuckets) {
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(Privilege(
        ResourcePattern::forAnySystemBucketsInAnyDatabase(sb_coll_test), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBuckets(), ActionType::find));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));
}

}  // namespace
}  // namespace mongo
