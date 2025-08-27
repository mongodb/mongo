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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/access_checks_gen.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_contract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session_test_fixture.h"
#include "mongo/db/auth/builtin_roles.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user_request_x509.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/query_cmd/release_memory_cmd.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class AuthorizationSessionTest : public AuthorizationSessionTestFixture {
public:
    using AuthorizationSessionTestFixture::AuthorizationSessionTestFixture;

    void testInvalidateUser();

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeFakePlanExecutor(
        OperationContext* _opCtx, NamespaceString nss) {
        // Create a mock ExpressionContext.
        auto expCtx = ExpressionContextBuilder{}.opCtx(_opCtx).ns(nss).build();
        auto workingSet = std::make_unique<WorkingSet>();
        auto queuedDataStage = std::make_unique<QueuedDataStage>(expCtx.get(), workingSet.get());
        return unittest::assertGet(
            plan_executor_factory::make(expCtx,
                                        std::move(workingSet),
                                        std::move(queuedDataStage),
                                        &CollectionPtr::null,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        QueryPlannerParams::DEFAULT,
                                        nss));
    }
};

class AuthorizationSessionTestWithoutAuth : public AuthorizationSessionTest {
public:
    AuthorizationSessionTestWithoutAuth()
        : AuthorizationSessionTest(Options{}.setAuthEnabled(false)) {}
};

const TenantId kTenantId1(OID("12345678901234567890aaaa"));
const TenantId kTenantId2(OID("12345678901234567890aaab"));

const NamespaceString testFooNss = NamespaceString::createNamespaceString_forTest("test.foo");
const NamespaceString testBarNss = NamespaceString::createNamespaceString_forTest("test.bar");
const NamespaceString testQuxNss = NamespaceString::createNamespaceString_forTest("test.qux");
const NamespaceString testTenant1FooNss =
    NamespaceString::createNamespaceString_forTest(kTenantId1, "test", "foo");
const NamespaceString testTenant1BarNss =
    NamespaceString::createNamespaceString_forTest(kTenantId1, "test", "bar");
const NamespaceString testTenant1QuxNss =
    NamespaceString::createNamespaceString_forTest(kTenantId1, "test", "qux");
const NamespaceString testTenant2FooNss =
    NamespaceString::createNamespaceString_forTest(kTenantId2, "test", "foo");

const DatabaseName testDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);
const DatabaseName otherDB = DatabaseName::createDatabaseName_forTest(boost::none, "other"_sd);
const DatabaseName ignoredDB = DatabaseName::createDatabaseName_forTest(boost::none, "ignored"_sd);

const ResourcePattern testDBResource = ResourcePattern::forDatabaseName(testDB);
const ResourcePattern otherDBResource = ResourcePattern::forDatabaseName(otherDB);
const ResourcePattern testFooCollResource(ResourcePattern::forExactNamespace(testFooNss));
const ResourcePattern testTenant1FooCollResource(
    ResourcePattern::forExactNamespace(testTenant1FooNss));
const ResourcePattern testTenant2FooCollResource(
    ResourcePattern::forExactNamespace(testTenant2FooNss));
const ResourcePattern otherFooCollResource(ResourcePattern::forExactNamespace(
    NamespaceString::createNamespaceString_forTest("other.foo")));
const ResourcePattern testUsersCollResource(ResourcePattern::forExactNamespace(
    NamespaceString::createNamespaceString_forTest("test.system.users")));
const ResourcePattern otherUsersCollResource(ResourcePattern::forExactNamespace(
    NamespaceString::createNamespaceString_forTest("other.system.users")));
const ResourcePattern testProfileCollResource(ResourcePattern::forExactNamespace(
    NamespaceString::createNamespaceString_forTest("test.system.profile")));
const ResourcePattern otherProfileCollResource(ResourcePattern::forExactNamespace(
    NamespaceString::createNamespaceString_forTest("other.system.profile")));

const UserName kUser1Test("user1"_sd, "test"_sd);
const std::unique_ptr<UserRequest> kUser1TestRequest =
    std::make_unique<UserRequestGeneral>(kUser1Test, boost::none);
const UserName kUser2Test("user2"_sd, "test"_sd);
const std::unique_ptr<UserRequest> kUser2TestRequest =
    std::make_unique<UserRequestGeneral>(kUser2Test, boost::none);
const UserName kTenant1UserTest("userTenant1"_sd, "test"_sd, kTenantId1);
const std::unique_ptr<UserRequest> kTenant1UserTestRequest =
    std::make_unique<UserRequestGeneral>(kTenant1UserTest, boost::none);
const UserName kTenant2UserTest("userTenant2"_sd, "test"_sd, kTenantId2);
const std::unique_ptr<UserRequest> kTenant2UserTestRequest =
    std::make_unique<UserRequestGeneral>(kTenant2UserTest, boost::none);

TEST_F(AuthorizationSessionTest, MultiAuthSameUserAllowed) {
    ASSERT_OK(createUser(kUser1Test, {}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));
    authzSession->logoutAllDatabases("Test finished");
}

TEST_F(AuthorizationSessionTest, MultiAuthSameDBDisallowed) {
    ASSERT_OK(createUser(kUser1Test, {}));
    ASSERT_OK(createUser(kUser2Test, {}));

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));
    ASSERT_NOT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser2TestRequest->clone(), boost::none));
    authzSession->logoutAllDatabases("Test finished");
}

TEST_F(AuthorizationSessionTest, MultiAuthMultiDBDisallowed) {
    ASSERT_OK(createUser(kUser1Test, {}));
    ASSERT_OK(createUser(kUser2Test, {}));

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));
    ASSERT_NOT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser2TestRequest->clone(), boost::none));
    authzSession->logoutAllDatabases("Test finished");
}

const auto kTestDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);
const auto kAdminDB = DatabaseName::createDatabaseName_forTest(boost::none, "admin"_sd);

const UserName kSpencerTest("spencer"_sd, kTestDB);
const std::unique_ptr<UserRequest> kSpencerTestRequest =
    std::make_unique<UserRequestGeneral>(kSpencerTest, boost::none);

const UserName kAdminAdmin("admin"_sd, kAdminDB);
const std::unique_ptr<UserRequest> kAdminAdminRequest =
    std::make_unique<UserRequestGeneral>(kAdminAdmin, boost::none);

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
    ASSERT_EQUALS(
        ErrorCodes::UserNotFound,
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));

    // Add a user with readWrite and dbAdmin on the test DB
    ASSERT_OK(createUser({"spencer", "test"}, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);

    // Add an admin user with readWriteAnyDatabase
    ASSERT_OK(createUser({"admin", "admin"}, {{"readWriteAnyDatabase", "admin"}}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kAdminAdminRequest->clone(), boost::none));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(
        ResourcePattern::forExactNamespace(
            NamespaceString::createNamespaceString_forTest("anydb.somecollection")),
        ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherDBResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::collMod));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::collMod));

    authzSession->logoutDatabase(kAdminDB, "Fire the admin!"_sd);
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
            Privilege(ResourcePattern::forDatabaseName(ignoredDB),
                      {ActionType::insert, ActionType::dbStats}),
            Privilege(ResourcePattern::forExactNamespace(
                          NamespaceString::createNamespaceString_forTest("ignored.ignored")),
                      {ActionType::insert, ActionType::collMod}),
        });

    authzSession->verifyContract(&ac);

    // Verify against a smaller contract that verifyContract fails
    AuthorizationContract acMissing(std::initializer_list<AccessCheckEnum>{},
                                    std::initializer_list<Privilege>{
                                        Privilege(ResourcePattern::forDatabaseName(ignoredDB),
                                                  {ActionType::insert, ActionType::dbStats}),
                                    });
    ASSERT_THROWS_CODE(authzSession->verifyContract(&acMissing), AssertionException, 5452401);
}

TEST_F(AuthorizationSessionTest, DuplicateRolesOK) {
    // Add a user with doubled-up readWrite and single dbAdmin on the test DB
    ASSERT_OK(createUser(kSpencerTest,
                         {{"readWrite", "test"}, {"dbAdmin", "test"}, {"readWrite", "test"}}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testDBResource, ActionType::dbStats));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(otherFooCollResource, ActionType::insert));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

const UserName kRWTest("rw"_sd, "test"_sd);
const UserName kUserAdminTest("useradmin"_sd, "test"_sd);
const UserName kRWAnyTest("rwany"_sd, "test"_sd);
const UserName kUserAdminAnyTest("useradminany"_sd, "test"_sd);

const std::unique_ptr<UserRequest> kRWTestRequest =
    std::make_unique<UserRequestGeneral>(kRWTest, boost::none);
const std::unique_ptr<UserRequest> kUserAdminTestRequest =
    std::make_unique<UserRequestGeneral>(kUserAdminTest, boost::none);
const std::unique_ptr<UserRequest> kRWAnyTestRequest =
    std::make_unique<UserRequestGeneral>(kRWAnyTest, boost::none);
const std::unique_ptr<UserRequest> kUserAdminAnyTestRequest =
    std::make_unique<UserRequestGeneral>(kUserAdminAnyTest, boost::none);

TEST_F(AuthorizationSessionTest, SystemCollectionsAccessControl) {
    ASSERT_OK(createUser(kRWTest, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(createUser(kUserAdminTest, {{"userAdmin", "test"}}));
    ASSERT_OK(createUser(kRWAnyTest,
                         {{"readWriteAnyDatabase", "admin"}, {"dbAdminAnyDatabase", "admin"}}));
    ASSERT_OK(createUser(kUserAdminAnyTest, {{"userAdminAnyDatabase", "admin"}}));

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kRWAnyTestRequest->clone(), boost::none));

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
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);

    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kUserAdminAnyTestRequest->clone(), boost::none));
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
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kRWTestRequest->clone(), boost::none));

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
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);

    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kUserAdminTestRequest->clone(), boost::none));
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
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

void AuthorizationSessionTest::testInvalidateUser() {
    transport::TransportLayerMock transportLayer;
    const std::shared_ptr<transport::Session> session =
        transport::MockSession::create(&transportLayer);
    auto& sslPeerInfo = SSLPeerInfo::forSession(session);
    sslPeerInfo = std::make_shared<SSLPeerInfo>();

    std::unique_ptr<UserRequest> userRequest = std::move(
        UserRequestX509::makeUserRequestX509(kSpencerTest, boost::none, sslPeerInfo).getValue());

    // Add a readWrite user
    ASSERT_OK(createUser(kSpencerTest, {{"readWrite", "test"}}));
    ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), std::move(userRequest), boost::none));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(kSpencerTest);
    ASSERT(user != nullptr);

    // Change the user to be read-only
    int ignored;
    ASSERT_OK(backendMock->remove(
        _opCtx.get(), NamespaceString::kAdminUsersNamespace, BSONObj(), BSONObj(), &ignored));
    ASSERT_OK(createUser(kSpencerTest, {{"read", "test"}}));

    // Make sure that invalidating the user causes the session to reload its privileges.
    AuthorizationManager::get(_opCtx->getService())->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    user = authzSession->lookupUser(kSpencerTest);
    ASSERT(user != nullptr);

    // Delete the user.
    ASSERT_OK(backendMock->remove(
        _opCtx.get(), NamespaceString::kAdminUsersNamespace, BSONObj(), BSONObj(), &ignored));
    // Make sure that invalidating the user causes the session to reload its privileges.
    AuthorizationManager::get(_opCtx->getService())->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    ASSERT_FALSE(authzSession->lookupUser(kSpencerTest));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

TEST_F(AuthorizationSessionTest, InvalidateUserByName) {
    // Basic test with SSLPeerInformation.
    testInvalidateUser();
}

TEST_F(AuthorizationSessionTest, UseOldUserInfoInFaceOfConnectivityProblems) {
    // Add a readWrite user
    ASSERT_OK(createUser({"spencer", "test"}, {{"readWrite", "test"}}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));

    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    User* user = authzSession->lookupUser(kSpencerTest);

    // Change the user to be read-only
    int ignored;
    backendMock->setFindsShouldFail(true);
    ASSERT_OK(backendMock->remove(
        _opCtx.get(), NamespaceString::kAdminUsersNamespace, BSONObj(), BSONObj(), &ignored));
    ASSERT_OK(createUser(kSpencerTest, {{"read", "test"}}));

    // Even though the user's privileges have been reduced, since we've configured user
    // document lookup to fail, the authz session should continue to use its known out-of-date
    // privilege data.
    AuthorizationManager::get(_opCtx->getService())->invalidateUserByName(user->getName());
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));

    // Once we configure document lookup to succeed again, authorization checks should
    // observe the new values.
    backendMock->setFindsShouldFail(false);
    authzSession->startRequest(_opCtx.get());  // Refreshes cached data for invalid users
    ASSERT_TRUE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::find));
    ASSERT_FALSE(
        authzSession->isAuthorizedForActionsOnResource(testFooCollResource, ActionType::insert));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

TEST_F(AuthorizationSessionTest, AcquireUserObtainsAndValidatesAuthenticationRestrictions) {
    ASSERT_OK(backendMock->insertUserDocument(
        _opCtx.get(),
        BSON("user" << "spencer"
                    << "db"
                    << "test"
                    << "credentials" << credentials << "roles"
                    << BSON_ARRAY(BSON("role" << "readWrite"
                                              << "db"
                                              << "test"))
                    << "authenticationRestrictions"
                    << BSON_ARRAY(
                           BSON("clientSource" << BSON_ARRAY("192.168.0.0/24" << "192.168.2.10")
                                               << "serverAddress" << BSON_ARRAY("192.168.0.2"))
                           << BSON("clientSource" << BSON_ARRAY("2001:DB8::1") << "serverAddress"
                                                  << BSON_ARRAY("2001:DB8::2"))
                           << BSON("clientSource" << BSON_ARRAY("127.0.0.1" << "::1")
                                                  << "serverAddress"
                                                  << BSON_ARRAY("127.0.0.1" << "::1")))),
        BSONObj()));


    auto assertWorks = [this](StringData clientSource, StringData serverAddress) {
        auto mock_session = std::make_shared<transport::MockSession>(
            HostAndPort(),
            SockAddr::create(clientSource, 5555, AF_UNSPEC),
            SockAddr::create(serverAddress, 27017, AF_UNSPEC),
            nullptr);
        auto client = getServiceContext()->getService()->makeClient("testClient", mock_session);
        auto opCtx = client->makeOperationContext();
        ASSERT_OK(authzSession->addAndAuthorizeUser(
            opCtx.get(), kSpencerTestRequest->clone(), boost::none));
        authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
    };

    auto assertFails = [this](StringData clientSource, StringData serverAddress) {
        auto mock_session = std::make_shared<transport::MockSession>(
            HostAndPort(),
            SockAddr::create(clientSource, 5555, AF_UNSPEC),
            SockAddr::create(serverAddress, 27017, AF_UNSPEC),
            nullptr);
        auto client = getServiceContext()->getService()->makeClient("testClient", mock_session);
        auto opCtx = client->makeOperationContext();
        ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(
            opCtx.get(), kSpencerTestRequest->clone(), boost::none));
    };

    // The empty RestrictionEnvironment will cause addAndAuthorizeUser to fail.
    ASSERT_NOT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));

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
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);
        auto nss = multitenancy ? testTenant1FooNss : testFooNss;

        auto aggReq = buildAggReq(nss, BSONArray());
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateEmptyPipelineWithFindAction) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        auto aggReq = buildAggReq(nss, BSONArray());
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithoutFindActionIfFirstStageNotIndexOrCollStats) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(
            Privilege(rsrc, {ActionType::indexStats, ActionType::collStats}), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                            << BSON("$indexStats" << BSONObj()));

        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateWithFindActionIfPipelineContainsIndexOrCollStats) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());
        BSONArray pipeline = BSON_ARRAY(BSON("$limit" << 1) << BSON("$collStats" << BSONObj())
                                                            << BSON("$indexStats" << BSONObj()));

        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCollStatsWithoutCollStatsAction) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateCollStatsWithCollStatsAction) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::collStats), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$collStats" << BSONObj()));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateIndexStatsWithoutIndexStatsAction) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateIndexStatsWithIndexStatsAction) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::indexStats), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$indexStats" << BSONObj()));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoD) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseWithoutInprogActionOnMongoS) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges =
            uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, true));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoD) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
        auto aggReq = buildAggReq(nss, pipeline);
        ASSERT_FALSE(authzSession->isAuthenticated());
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersFalseIfNotAuthenticatedOnMongoS) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false)));
        auto aggReq = buildAggReq(nss, pipeline);

        PrivilegeVector privileges =
            uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, true));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoD) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateCurrentOpAllUsersTrueWithoutInprogActionOnMongoS) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges =
            uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, true));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoD) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(
            Privilege(ResourcePattern::forClusterResource(nss.tenantId()), ActionType::inprog),
            nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateCurrentOpAllUsersTrueWithInprogActionOnMongoS) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(
            Privilege(ResourcePattern::forClusterResource(nss.tenantId()), ActionType::inprog),
            nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges =
            uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, true));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoD) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline =
            BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotSpoofAllUsersTrueWithoutInprogActionOnMongoS) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline =
            BSON_ARRAY(BSON("$currentOp" << BSON("allUsers" << false << "allUsers" << true)));
        auto aggReq = buildAggReq(nss, pipeline);
        PrivilegeVector privileges =
            uassertStatusOK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, true));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, AddPrivilegesForStageFailsIfOutNamespaceIsNotValid) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        authzSession->assumePrivilegesForDB(Privilege(rsrc, ActionType::find), nss.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$out" << ""));
        auto aggReq = buildAggReq(nss, pipeline);
        ASSERT_THROWS_CODE(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq, false),
                           AssertionException,
                           ErrorCodes::InvalidNamespace);
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateOutWithoutInsertAndRemoveOnTargetNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        // We only have find on the aggregation namespace.
        authzSession->assumePrivilegesForDB(Privilege(rsrcFoo, ActionType::find), nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$out" << nssBar.coll()));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), testFooNss, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

        // We have insert but not remove on the $out namespace.
        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::insert)},
            nssBar.dbName());
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

        // We have remove but not insert on the $out namespace.
        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::remove)},
            nssBar.dbName());
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateOutWithInsertAndRemoveOnTargetNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find),
             Privilege(rsrcBar, {ActionType::insert, ActionType::remove})},
            nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$out" << nssBar.coll()));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));

        auto aggNoBypassDocumentValidationReq =
            buildAggReq(nssFoo, pipeline, false /* bypassDocValidation*/);

        privileges = uassertStatusOK(auth::getPrivilegesForAggregate(
            authzSession.get(), nssFoo, aggNoBypassDocumentValidationReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateOutBypassingValidationWithoutBypassDocumentValidationOnTargetNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find),
             Privilege(rsrcBar, {ActionType::insert, ActionType::remove})},
            nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$out" << nssBar.coll()));
        auto aggReq = buildAggReq(nssFoo, pipeline, true /* bypassDocValidation*/);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest,
       CanAggregateOutBypassingValidationWithBypassDocumentValidationOnTargetNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find),
             Privilege(
                 rsrcBar,
                 {ActionType::insert, ActionType::remove, ActionType::bypassDocumentValidation})},
            nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$out" << nssBar.coll()));
        auto aggReq = buildAggReq(nssFoo, pipeline, true /* bypassDocValidation*/);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, true));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;

        authzSession->assumePrivilegesForDB(Privilege(rsrcFoo, ActionType::find), nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << nssBar.coll())));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::find)},
            nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << nssBar.coll())));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, true));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}


TEST_F(AuthorizationSessionTest, CannotAggregateLookupWithoutFindOnNestedJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);
        auto nssQux = multitenancy ? testTenant1QuxNss : testQuxNss;

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::find)},
            nssFoo.dbName());

        BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << nssQux.coll())));
        BSONArray pipeline = BSON_ARRAY(
            BSON("$lookup" << BSON("from" << nssBar.coll() << "pipeline" << nestedPipeline)));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateLookupWithFindOnNestedJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);
        auto nssQux = multitenancy ? testTenant1QuxNss : testQuxNss;
        auto rsrcQux = ResourcePattern::forExactNamespace(nssQux);

        authzSession->assumePrivilegesForDB({Privilege(rsrcFoo, ActionType::find),
                                             Privilege(rsrcBar, ActionType::find),
                                             Privilege(rsrcQux, ActionType::find)},
                                            nssFoo.dbName());

        BSONArray nestedPipeline = BSON_ARRAY(BSON("$lookup" << BSON("from" << nssQux.coll())));
        BSONArray pipeline = BSON_ARRAY(
            BSON("$lookup" << BSON("from" << nssBar.coll() << "pipeline" << nestedPipeline)));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CheckAuthForAggregateWithDeeplyNestedLookup) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);

        authzSession->assumePrivilegesForDB(Privilege(rsrcFoo, ActionType::find), nssFoo.dbName());

        // Recursively adds nested $lookup stages to 'pipelineBob', building a pipeline with
        // 'levelsToGo' deep $lookup stages.
        std::function<void(BSONArrayBuilder*, int)> addNestedPipeline;
        addNestedPipeline = [&addNestedPipeline, &nssFoo](BSONArrayBuilder* pipelineBob,
                                                          int levelsToGo) {
            if (levelsToGo == 0) {
                return;
            }

            BSONObjBuilder objectBob(pipelineBob->subobjStart());
            BSONObjBuilder lookupBob(objectBob.subobjStart("$lookup"));
            lookupBob << "from" << nssFoo.coll() << "as"
                      << "as";
            BSONArrayBuilder subPipelineBob(lookupBob.subarrayStart("pipeline"));
            addNestedPipeline(&subPipelineBob, --levelsToGo);
            subPipelineBob.doneFast();
            lookupBob.doneFast();
            objectBob.doneFast();
        };

        // checkAuthForAggregate() should succeed for an aggregate command that has a deeply nested
        // $lookup sub-pipeline chain. Each nested $lookup stage adds 3 to the depth of the command
        // object. We set 'maxLookupDepth' depth to allow for a command object that is at or just
        // under max BSONDepth.
        const uint32_t aggregateCommandDepth = 1;
        const uint32_t lookupDepth = 3;
        const uint32_t maxLookupDepth =
            (BSONDepth::getMaxAllowableDepth() - aggregateCommandDepth) / lookupDepth;

        BSONObjBuilder cmdBuilder;
        cmdBuilder << "aggregate" << nssFoo.coll();
        BSONArrayBuilder pipelineBuilder(cmdBuilder.subarrayStart("pipeline"));
        addNestedPipeline(&pipelineBuilder, maxLookupDepth);
        pipelineBuilder.doneFast();
        cmdBuilder << "cursor" << BSONObj() << "$db" << nssFoo.db_forTest();

        auto aggReq = uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
            cmdBuilder.obj(), makeVTS(nssFoo), boost::none));
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}


TEST_F(AuthorizationSessionTest, CannotAggregateGraphLookupWithoutFindOnJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;

        authzSession->assumePrivilegesForDB(Privilege(rsrcFoo, ActionType::find), nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << nssBar.coll())));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, CanAggregateGraphLookupWithFindOnJoinedNamespace) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::find)},
            nssFoo.dbName());

        BSONArray pipeline = BSON_ARRAY(BSON("$graphLookup" << BSON("from" << nssBar.coll())));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest,
       CannotAggregateFacetWithLookupAndGraphLookupWithoutFindOnJoinedNamespaces) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);

        // We only have find on the aggregation namespace.
        authzSession->assumePrivilegesForDB(Privilege(rsrcFoo, ActionType::find), nssFoo.dbName());

        BSONArray pipeline =
            BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                                "[{$graphLookup: {from: 'qux'}}]}}"));
        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, false));
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

        // We have find on the $lookup namespace but not on the $graphLookup namespace.
        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::find)},
            nssFoo.dbName());
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));

        // We have find on the $graphLookup namespace but not on the $lookup namespace.
        authzSession->assumePrivilegesForDB(
            {Privilege(rsrcFoo, ActionType::find), Privilege(rsrcBar, ActionType::find)},
            nssFoo.dbName());
        ASSERT_FALSE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest,
       CanAggregateFacetWithLookupAndGraphLookupWithFindOnJoinedNamespaces) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);

        auto nssFoo = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrcFoo = ResourcePattern::forExactNamespace(nssFoo);
        auto nssBar = multitenancy ? testTenant1BarNss : testBarNss;
        auto rsrcBar = ResourcePattern::forExactNamespace(nssBar);
        auto nssQux = multitenancy ? testTenant1QuxNss : testQuxNss;
        auto rsrcQux = ResourcePattern::forExactNamespace(nssQux);

        authzSession->assumePrivilegesForDB({Privilege(rsrcFoo, ActionType::find),
                                             Privilege(rsrcBar, ActionType::find),
                                             Privilege(rsrcQux, ActionType::find)},
                                            nssFoo.dbName());

        BSONArray pipeline =
            BSON_ARRAY(fromjson("{$facet: {lookup: [{$lookup: {from: 'bar'}}], graphLookup: "
                                "[{$graphLookup: {from: 'qux'}}]}}"));

        auto aggReq = buildAggReq(nssFoo, pipeline);
        PrivilegeVector privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(authzSession.get(), nssFoo, aggReq, true));
        ASSERT_TRUE(authzSession->isAuthorizedForPrivileges(privileges));
    }
}

TEST_F(AuthorizationSessionTest, UnauthorizedSessionIsCoauthorizedWithNobody) {
    ASSERT_TRUE(authzSession->isCoauthorizedWith(boost::none));
}

TEST_F(AuthorizationSessionTest, UnauthorizedSessionIsNotCoauthorizedWithAnybody) {
    authzManager->setAuthEnabled(true);
    ASSERT_FALSE(authzSession->isCoauthorizedWith(kSpencerTest));
}

TEST_F(AuthorizationSessionTestWithoutAuth,
       UnauthorizedSessionIsCoauthorizedWithAnybodyWhenAuthIsDisabled) {
    ASSERT_FALSE(authzManager->isAuthEnabled());
    ASSERT_TRUE(authzSession->isCoauthorizedWith(kSpencerTest));
}

TEST_F(AuthorizationSessionTest, AuthorizedSessionIsNotCoauthorizedNobody) {
    authzManager->setAuthEnabled(true);
    ASSERT_OK(createUser(kSpencerTest, {}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));
    ASSERT_FALSE(authzSession->isCoauthorizedWith(boost::none));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

TEST_F(AuthorizationSessionTestWithoutAuth,
       AuthorizedSessionIsCoauthorizedNobodyWhenAuthIsDisabled) {
    ASSERT_FALSE(authzManager->isAuthEnabled());
    ASSERT_OK(createUser(kSpencerTest, {}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));
    ASSERT_TRUE(authzSession->isCoauthorizedWith(kSpencerTest));
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
}

const auto listTestCollectionsPayload = BSON("listCollections"_sd << 1 << "$db"
                                                                  << "test"_sd);
const auto listTestCollectionsCmd =
    ListCollections::parse(listTestCollectionsPayload, IDLParserContext("listTestCollectionsCmd"));
const auto listOtherCollectionsPayload = BSON("listCollections"_sd << 1 << "$db"
                                                                   << "other"_sd);
const auto listOtherCollectionsCmd = ListCollections::parse(
    listOtherCollectionsPayload, IDLParserContext("listOtherCollectionsCmd"));
const auto listOwnTestCollectionsPayload =
    BSON("listCollections"_sd << 1 << "$db"
                              << "test"_sd
                              << "nameOnly"_sd << true << "authorizedCollections"_sd << true);
const auto listOwnTestCollectionsCmd = ListCollections::parse(
    listOwnTestCollectionsPayload, IDLParserContext("listOwnTestCollectionsCmd"));

TEST_F(AuthorizationSessionTest, CannotListCollectionsWithoutListCollectionsPrivilege) {
    // With no privileges, there is no authorization to list collections
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(listTestCollectionsCmd).getStatus());
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(listOtherCollectionsCmd).getStatus());
}

TEST_F(AuthorizationSessionTest, CanListCollectionsWithListCollectionsPrivilege) {
    // The listCollections privilege authorizes the list collections command on the named database
    // only.
    authzSession->assumePrivilegesForDB(Privilege(testDBResource, ActionType::listCollections),
                                        testDB);

    // "test" DB is okay.
    ASSERT_OK(authzSession->checkAuthorizedToListCollections(listTestCollectionsCmd).getStatus());

    // "other" DB is not.
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(listOtherCollectionsCmd).getStatus());
}

TEST_F(AuthorizationSessionTest, CanListOwnCollectionsWithPrivilege) {
    // Any privilege on a DB implies authorization to list one's own collections.
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find), testDB);

    // Just own collections is okay.
    ASSERT_OK(
        authzSession->checkAuthorizedToListCollections(listOwnTestCollectionsCmd).getStatus());

    // All collections is not.
    ASSERT_EQ(ErrorCodes::Unauthorized,
              authzSession->checkAuthorizedToListCollections(listTestCollectionsCmd).getStatus());
}

const auto kAnyResource = ResourcePattern::forAnyResource(boost::none);
const auto kAnyNormalResource = ResourcePattern::forAnyNormalResource(boost::none);
const auto kAnySystemBucketResource = ResourcePattern::forAnySystemBuckets(boost::none);

TEST_F(AuthorizationSessionTest, CanCheckIfHasAnyPrivilegeOnResource) {
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));

    // If we have a collection privilege, we have actions on that collection
    authzSession->assumePrivilegesForDB(Privilege(testFooCollResource, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_FALSE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forDatabaseName(testDB)));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));

    // If we have a database privilege, we have actions on that database and all collections it
    // contains
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forDatabaseName(testDB), ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_TRUE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forDatabaseName(testDB)));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));

    // If we have a privilege on anyNormalResource, we have actions on all databases and all
    // collections they contain
    authzSession->assumePrivilegesForDB(Privilege(kAnyNormalResource, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testFooCollResource));
    ASSERT_TRUE(
        authzSession->isAuthorizedForAnyActionOnResource(ResourcePattern::forDatabaseName(testDB)));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));
}

TEST_F(AuthorizationSessionTest, CanUseUUIDNamespacesWithPrivilege) {
    BSONObj stringObj = BSON("a" << "string");
    BSONObj uuidObj = BSON("a" << UUID::gen());

    // Strings require no privileges
    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(stringObj.firstElement()));

    // UUIDs cannot be parsed with default privileges
    ASSERT_FALSE(authzSession->isAuthorizedToParseNamespaceElement(uuidObj.firstElement()));

    // The useUUID privilege allows UUIDs to be parsed
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useUUID), testDB);

    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(stringObj.firstElement()));
    ASSERT_TRUE(authzSession->isAuthorizedToParseNamespaceElement(uuidObj.firstElement()));

    // Verify we recorded the all the auth checks correctly
    AuthorizationContract ac(
        std::initializer_list<AccessCheckEnum>{
            AccessCheckEnum::kIsAuthorizedToParseNamespaceElement},
        std::initializer_list<Privilege>{
            Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useUUID)});

    authzSession->verifyContract(&ac);
}

const UserName kGMarksAdmin("gmarks", "admin");
const std::unique_ptr<UserRequest> kGMarksAdminRequest =
    std::make_unique<UserRequestGeneral>(kGMarksAdmin, boost::none);

TEST_F(AuthorizationSessionTest, MayBypassWriteBlockingModeIsSetCorrectly) {
    authzManager->setAuthEnabled(true);
    ASSERT_FALSE(authzSession->mayBypassWriteBlockingMode());

    // Add a user without the restore role and ensure we can't bypass
    ASSERT_OK(backendMock->insertUserDocument(_opCtx.get(),
                                              BSON("user" << "spencer"
                                                          << "db"
                                                          << "test"
                                                          << "credentials" << credentials << "roles"
                                                          << BSON_ARRAY(BSON("role" << "readWrite"
                                                                                    << "db"
                                                                                    << "test"))),
                                              BSONObj()));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));
    ASSERT_FALSE(authzSession->mayBypassWriteBlockingMode());

    // Add a user with restore role on admin db and ensure we can bypass
    ASSERT_OK(backendMock->insertUserDocument(_opCtx.get(),
                                              BSON("user" << "gmarks"
                                                          << "db"
                                                          << "admin"
                                                          << "credentials" << credentials << "roles"
                                                          << BSON_ARRAY(BSON("role" << "restore"
                                                                                    << "db"
                                                                                    << "admin"))),
                                              BSONObj()));
    authzSession->logoutDatabase(kTestDB, "End of test"_sd);

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kGMarksAdminRequest->clone(), boost::none));
    ASSERT_TRUE(authzSession->mayBypassWriteBlockingMode());

    // Remove that user by logging out of the admin db and ensure we can't bypass anymore
    authzSession->logoutDatabase(kAdminDB, ""_sd);
    ASSERT_FALSE(authzSession->mayBypassWriteBlockingMode());

    // Add a user with the root role, which should confer restore role for cluster resource, and
    // ensure we can bypass
    ASSERT_OK(backendMock->insertUserDocument(_opCtx.get(),
                                              BSON("user" << "admin"
                                                          << "db"
                                                          << "admin"
                                                          << "credentials" << credentials << "roles"
                                                          << BSON_ARRAY(BSON("role" << "root"
                                                                                    << "db"
                                                                                    << "admin"))),
                                              BSONObj()));
    authzSession->logoutDatabase(kAdminDB, ""_sd);

    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kAdminAdminRequest->clone(), boost::none));
    ASSERT_TRUE(authzSession->mayBypassWriteBlockingMode());

    // Remove non-privileged user by logging out of test db and ensure we can still bypass
    authzSession->logoutDatabase(kTestDB, ""_sd);
    ASSERT_TRUE(authzSession->mayBypassWriteBlockingMode());

    // Remove privileged user by logging out of admin db and ensure we cannot bypass
    authzSession->logoutDatabase(kAdminDB, ""_sd);
    ASSERT_FALSE(authzSession->mayBypassWriteBlockingMode());
}

TEST_F(AuthorizationSessionTest, InvalidExpirationTime) {
    // Create and authorize valid user with invalid expiration.
    Date_t expirationTime = clockSource()->now() - Hours(1);
    ASSERT_OK(createUser({"spencer", "test"}, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kSpencerTestRequest->clone(), expirationTime));
}

TEST_F(AuthorizationSessionTest, NoExpirationTime) {
    // Create and authorize valid user with no expiration.
    ASSERT_OK(createUser({"spencer", "test"}, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kSpencerTestRequest->clone(), boost::none));
    assertActive(testFooCollResource, ActionType::insert);

    // Assert that moving the clock forward has no impact on a session without expiration time.
    clockSource()->advance(Hours(24));
    authzSession->startRequest(_opCtx.get());
    assertActive(testFooCollResource, ActionType::insert);

    // Assert that logout occurs normally.
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
    assertLogout(testFooCollResource, ActionType::insert);
}

TEST_F(AuthorizationSessionTest, TenantSeparation) {
    const UserName readWriteAnyDBUser = {"rwanyuser", "test"};
    std::unique_ptr<UserRequest> readWriteAnyDBUserRequest =
        std::make_unique<UserRequestGeneral>(readWriteAnyDBUser, boost::none);
    const UserName tenant2SystemUser = {"gmarks", "test", kTenantId2};
    std::unique_ptr<UserRequest> tenant2SystemUserRequest =
        std::make_unique<UserRequestGeneral>(tenant2SystemUser, boost::none);
    const UserName systemUser = {"spencer", "test"};
    std::unique_ptr<UserRequest> systemUserRequest =
        std::make_unique<UserRequestGeneral>(systemUser, boost::none);

    auto testSystemRolesResource = ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("test", "system.roles"));
    auto testSystemRolesTenant2Resource = ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest(kTenantId2, "test", "system.roles"));

    ASSERT_OK(createUser(kTenant1UserTest, {{"readWrite", "test"}}));
    ASSERT_OK(createUser(kTenant2UserTest, {{"readWriteAnyDatabase", "admin"}}));
    ASSERT_OK(createUser(kUser1Test, {{"readWrite", "test"}}));
    ASSERT_OK(createUser(kUser2Test, {{"root", "admin"}}));
    ASSERT_OK(createUser(readWriteAnyDBUser, {{"readWriteAnyDatabase", "admin"}}));
    ASSERT_OK(createUser(tenant2SystemUser, {{"__system", "admin"}}));
    ASSERT_OK(createUser(systemUser, {{"__system", "admin"}}));

    // User with tenant ID #1 with basic read/write privileges on "test" should be able to write to
    // tenant ID #1's test collection, and no others.
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kTenant1UserTestRequest->clone(), boost::none));
    assertActive(testTenant1FooCollResource, ActionType::insert);
    assertNotAuthorized(testFooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTenant1UserTestRequest->getUserName().getDatabaseName(),
                                 "Log out tenant 1 for test"_sd);
    assertLogout(testTenant1FooCollResource, ActionType::insert);

    // User with tenant ID #2 with readWriteAny should be able to write to any of tenant ID #2's
    // normal collections, and no others.
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kTenant2UserTestRequest->clone(), boost::none));
    assertActive(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testFooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant1FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTenant2UserTestRequest->getUserName().getDatabaseName(),
                                 "Log out tenant 2 for test"_sd);
    assertLogout(testTenant2FooCollResource, ActionType::insert);

    // User with no tenant ID with basic read/write privileges on "test" should be able to write to
    // the no-tenant test collection, and no others.
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));
    assertActive(testFooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant1FooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTestDB, "Log out user 1 for test"_sd);
    assertLogout(testFooCollResource, ActionType::insert);

    // User with no tenant ID with root should be able to write to any tenant's normal
    // collections, because boost::none acts as "any tenant" for privileges which don't specify a
    // namespace/DB, and root has the useTenant privilege.
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kUser2TestRequest->clone(), boost::none));
    assertActive(testFooCollResource, ActionType::insert);
    assertActive(testTenant1FooCollResource, ActionType::insert);
    assertActive(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTestDB, "Log out user 2 for test"_sd);
    assertLogout(testFooCollResource, ActionType::insert);

    // User with no tenant ID with readWriteAnyDatabase should be able to write to normal
    // collections with no tenant ID, because readWriteAnyDatabase lacks the useTenant privilege and
    // thus can't read/write to other tenants' databases.
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), std::move(readWriteAnyDBUserRequest), boost::none));
    assertActive(testFooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant1FooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTestDB, "Log out read/write any DB user for test"_sd);
    assertLogout(testFooCollResource, ActionType::insert);

    // User with tenant ID 2 with __system privileges should be able to write to any of tenant 2's
    // collections, including system collections.
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), tenant2SystemUserRequest->clone(), boost::none));
    assertActive(testTenant2FooCollResource, ActionType::insert);
    assertNotAuthorized(testFooCollResource, ActionType::insert);
    assertNotAuthorized(testTenant1FooCollResource, ActionType::insert);
    assertNotAuthorized(testSystemRolesResource, ActionType::insert);
    assertActive(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(tenant2SystemUserRequest->getUserName().getDatabaseName(),
                                 "Log out tenant 2 system user for test"_sd);
    assertLogout(testTenant2FooCollResource, ActionType::insert);

    // User with no tenant ID with __system privileges should be able to write to any tenant's
    // collections.
    ASSERT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), std::move(systemUserRequest), boost::none));
    assertActive(testFooCollResource, ActionType::insert);
    assertActive(testTenant1FooCollResource, ActionType::insert);
    assertActive(testTenant2FooCollResource, ActionType::insert);
    assertActive(testSystemRolesResource, ActionType::insert);
    assertActive(testSystemRolesTenant2Resource, ActionType::insert);

    authzSession->logoutDatabase(kTestDB, "Log out system user for test"_sd);
    assertLogout(testFooCollResource, ActionType::insert);
}

TEST_F(AuthorizationSessionTest, ExpiredSessionWithReauth) {
    // Tests authorization session flow from unauthenticated to active to expired to active (reauth)
    // to expired to logged out.

    // Create and authorize a user with a valid expiration time set in the future.
    Date_t expirationTime = clockSource()->now() + Hours(1);
    ASSERT_OK(createUser({"spencer", "test"}, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(createUser({"admin", "admin"}, {{"readWriteAnyDatabase", "admin"}}));
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kSpencerTestRequest->clone(), expirationTime));

    // Assert that advancing the clock by 30 minutes does not trigger expiration.
    auto clock = clockSource();
    clock->advance(Minutes(30));
    authzSession->startRequest(
        _opCtx.get());  // Refreshes session's authentication state based on expiration.
    assertActive(testFooCollResource, ActionType::insert);

    // Assert that the session is now expired and subsequently is no longer authenticated or
    // authorized to do anything after fast-forwarding the clock source.
    clock->advance(Hours(2));
    authzSession->startRequest(
        _opCtx.get());  // Refreshes session's authentication state based on expiration.
    assertExpired(testFooCollResource, ActionType::insert);

    // Authorize the same user again to simulate re-login.
    expirationTime += Hours(2);
    ASSERT_OK(authzSession->addAndAuthorizeUser(
        _opCtx.get(), kSpencerTestRequest->clone(), expirationTime));
    assertActive(testFooCollResource, ActionType::insert);

    // Expire the user again, this time by setting clock to the exact expiration time boundary.
    clock->reset(expirationTime);
    authzSession->startRequest(_opCtx.get());
    assertExpired(testFooCollResource, ActionType::insert);

    // Assert that a different user cannot log in on the expired connection.
    ASSERT_NOT_OK(
        authzSession->addAndAuthorizeUser(_opCtx.get(), kAdminAdminRequest->clone(), boost::none));
    assertExpired(testFooCollResource, ActionType::insert);

    // Check that explicit logout from an expired connection works as expected.
    authzSession->logoutDatabase(kTestDB, "Kill the test!"_sd);
    assertLogout(ResourcePattern::forExactNamespace(
                     NamespaceString::createNamespaceString_forTest("anydb.somecollection")),
                 ActionType::insert);
}


TEST_F(AuthorizationSessionTest, ExpirationWithSecurityTokenNOK) {
    constexpr auto kVTSKey = "secret"_sd;
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          kVTSKey);

    // Tests authorization flow from unauthenticated to active (via token) to unauthenticated to
    // active (via stateful connection) to unauthenticated.

    // Create and authorize a security token user.
    ASSERT_OK(createUser(kTenant1UserTest, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(createUser(kUser1Test, {{"readWriteAnyDatabase", "admin"}}));
    ASSERT_OK(createUser(kTenant2UserTest, {{"readWriteAnyDatabase", "admin"}}));

    {
        auth::ValidatedTenancyScope validatedTenancyScope =
            auth::ValidatedTenancyScopeFactory::create(
                kTenant1UserTest,
                kVTSKey,
                auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

        // Actual expiration used by AuthorizationSession will be the minimum of
        // the token's known expiraiton time and the expiration time passed in.
        const auto checkExpiration = [&](const boost::optional<Date_t>& expire,
                                         const Date_t& expect) {
            auth::ValidatedTenancyScope::set(_opCtx.get(), validatedTenancyScope);
            ASSERT_OK(authzSession->addAndAuthorizeUser(
                _opCtx.get(), kTenant1UserTestRequest->clone(), expire));
            ASSERT_EQ(authzSession->getExpiration(), expect);

            // Reset for next test.
            auth::ValidatedTenancyScope::set(_opCtx.get(), boost::none);
            authzSession->startRequest(_opCtx.get());
            assertLogout(testTenant1FooCollResource, ActionType::insert);
        };
        const auto exp = validatedTenancyScope.getExpiration();
        checkExpiration(boost::none, exp);    // Uses token's expiration
        checkExpiration(Date_t::max(), exp);  // Longer expiration does not override token.
        checkExpiration(exp - Seconds{1}, exp - Seconds{1});  // Shorter expiration does.
    }

    {
        auth::ValidatedTenancyScope validatedTenancyScope =
            auth::ValidatedTenancyScopeFactory::create(
                kTenant1UserTest,
                kVTSKey,
                auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});

        // Perform authentication checks.
        auth::ValidatedTenancyScope::set(_opCtx.get(), validatedTenancyScope);
        ASSERT_OK(authzSession->addAndAuthorizeUser(
            _opCtx.get(), kTenant1UserTestRequest->clone(), boost::none));

        // Assert that the session is authenticated and authorized as expected.
        assertSecurityToken(testTenant1FooCollResource, ActionType::insert);

        // Since user has a tenantId, we expect it should only have access to its own collections.
        assertNotAuthorized(testFooCollResource, ActionType::insert);
        assertNotAuthorized(testTenant2FooCollResource, ActionType::insert);

        // Assert that another user can't be authorized while the security token is auth'd.
        ASSERT_NOT_OK(authzSession->addAndAuthorizeUser(
            _opCtx.get(), kUser1TestRequest->clone(), boost::none));

        // Check that starting a new request without the security token decoration results in token
        // user logout.
        auth::ValidatedTenancyScope::set(_opCtx.get(), boost::none);
        authzSession->startRequest(_opCtx.get());
        assertLogout(testTenant1FooCollResource, ActionType::insert);

        // Assert that a connection-based user with an expiration policy can be authorized after
        // token logout.
        const auto kSomeCollNss = NamespaceString::createNamespaceString_forTest(
            boost::none, "anydb"_sd, "somecollection"_sd);
        const auto kSomeCollRsrc = ResourcePattern::forExactNamespace(kSomeCollNss);
        ASSERT_OK(authzSession->addAndAuthorizeUser(
            _opCtx.get(), kUser1TestRequest->clone(), Date_t() + Hours{1}));
        assertActive(kSomeCollRsrc, ActionType::insert);

        // Check that logout proceeds normally.
        authzSession->logoutDatabase(kTestDB, "Log out readWriteAny user for test"_sd);
        assertLogout(kSomeCollRsrc, ActionType::insert);
    }

    // Create a new validated tenancy scope for the readWriteAny tenant user.
    {
        auth::ValidatedTenancyScope validatedTenancyScope =
            auth::ValidatedTenancyScopeFactory::create(
                kTenant2UserTest,
                kVTSKey,
                auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});
        auth::ValidatedTenancyScope::set(_opCtx.get(), validatedTenancyScope);
        auth::ValidatedTenancyScope::set(_opCtx.get(), validatedTenancyScope);

        ASSERT_OK(authzSession->addAndAuthorizeUser(
            _opCtx.get(), kTenant2UserTestRequest->clone(), boost::none));

        // Ensure that even though it has the readWriteAny role, this user only has privileges on
        // collections with matching tenant ID.
        assertSecurityToken(testTenant2FooCollResource, ActionType::insert);

        assertNotAuthorized(testFooCollResource, ActionType::insert);
        assertNotAuthorized(testTenant1FooCollResource, ActionType::insert);

        // Check that starting a new request without the security token decoration results in token
        // user logout.
        auth::ValidatedTenancyScope::set(_opCtx.get(), boost::none);
        authzSession->startRequest(_opCtx.get());
        assertLogout(testTenant2FooCollResource, ActionType::insert);
    }
}

TEST_F(AuthorizationSessionTest, CheckBuiltInRolesForBypassDefaultMaxTimeMS) {
    // Verify the "root" role is authorised  for 'bypassDefaultMaxTimeMS'.
    authzSession->assumePrivilegesForBuiltinRole(RoleName{"root", "admin"});
    ASSERT_TRUE(authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS,
                                                           boost::none));

    authzSession->logoutAllDatabases("Test finished");

    // Verify the "__system" role is authorised  for 'bypassDefaultMaxTimeMS'.
    authzSession->grantInternalAuthorization();
    ASSERT_TRUE(authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS,
                                                           boost::none));
}

TEST_F(AuthorizationSessionTest, CheckAuthorizationForReleaseMemoryAuthorizedUser) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);
    authzManager->setAuthEnabled(true);

    UserName username("spencer", "admin", boost::none);
    const std::unique_ptr<UserRequest> usernameTestRequest =
        std::make_unique<UserRequestGeneral>(username, boost::none);

    // Get the authorization session that will be used when running the command.
    auto authzSessionForCommand = AuthorizationSession::get(_opCtx->getClient());
    ASSERT_OK(createUser(username, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(authzSessionForCommand->addAndAuthorizeUser(
        _opCtx.get(), usernameTestRequest->clone(), boost::none));


    // Assume privileges for the database to allow releaseMemoryAnyCursor action.
    PrivilegeVector privileges;
    auth::generateUniversalPrivileges(&privileges, boost::none /* tenantId */);
    auto user = authzSessionForCommand->getAuthenticatedUser();
    ASSERT(user != boost::none);
    (*user)->addPrivileges(privileges);
    ASSERT_TRUE(authzSessionForCommand->isAuthorizedForActionsOnResource(
        CommandHelpers::resourcePatternForNamespace(testFooNss),
        ActionType::releaseMemoryAnyCursor));

    // Get the cursor manager and register a cursor. We need to detach it from the operation context
    // to allow releaseMemory to reattach it later during execution.
    CursorManager* cursorManager = CursorManager::get(_opCtx.get());
    auto exec = makeFakePlanExecutor(_opCtx.get(), testFooNss);
    exec->saveState();
    exec->detachFromOperationContext();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(),
        {
            std::move(exec),
            testFooNss,
            username,
            APIParameters(),
            _opCtx->getWriteConcern(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            BSONObj(),
            PrivilegeVector(),
        });
    const auto cursorId = cursorPin->cursorid();
    cursorPin.release();

    const IDLParserContext ctxt("releaseMemory");
    auto bsonObj = BSON("releaseMemory" << BSON_ARRAY(cursorId) << "$db"
                                        << "test");
    ReleaseMemoryCommandRequest request = ReleaseMemoryCommandRequest::parse(bsonObj, ctxt);
    ReleaseMemoryCmd commandInstance;
    ReleaseMemoryCmd::Invocation invocation(_opCtx.get(), &commandInstance, request.serialize());
    ReleaseMemoryCommandReply reply = invocation.typedRun(_opCtx.get());

    // We expect the cursor to be released successfully.
    ASSERT_EQ(reply.getCursorsReleased().size(), 1);
    ASSERT_EQ(reply.getCursorsReleased()[0], cursorId);
    ASSERT_TRUE(reply.getCursorsNotFound().empty());
    ASSERT_TRUE(reply.getCursorsCurrentlyPinned().empty());
}

TEST_F(AuthorizationSessionTest, CheckAuthorizationForReleaseMemoryUnauthorizedUser) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);
    authzManager->setAuthEnabled(true);
    UserName usernameUnauth("fakeSpencer", "admin");

    // Get the cursor manager and register a cursor. We need to detach it from the operation context
    // to allow releaseMemory to reattach it later during execution.
    CursorManager* cursorManager = CursorManager::get(_opCtx.get());
    auto exec = makeFakePlanExecutor(_opCtx.get(), testFooNss);
    exec->saveState();
    exec->detachFromOperationContext();

    auto cursorPin = cursorManager->registerCursor(
        _opCtx.get(),
        {
            std::move(exec),
            testFooNss,
            usernameUnauth,
            APIParameters(),
            _opCtx->getWriteConcern(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            BSONObj(),
            PrivilegeVector(),
        });
    const auto cursorId = cursorPin->cursorid();
    cursorPin.release();

    const IDLParserContext ctxt("releaseMemory");
    auto bsonObj = BSON("releaseMemory" << BSON_ARRAY(cursorId) << "$db"
                                        << "test");
    ReleaseMemoryCommandRequest request = ReleaseMemoryCommandRequest::parse(bsonObj, ctxt);
    ReleaseMemoryCmd commandInstance;
    ReleaseMemoryCmd::Invocation invocation(_opCtx.get(), &commandInstance, request.serialize());
    ASSERT_THROWS_CODE(invocation.typedRun(_opCtx.get()), DBException, ErrorCodes::Unauthorized);
}

TEST_F(AuthorizationSessionTest, CheckAuthorizationForKillCursorsAuthorizedUser) {
    authzManager->setAuthEnabled(true);

    UserName username("spencer", "admin", boost::none);
    const std::unique_ptr<UserRequest> usernameTestRequest =
        std::make_unique<UserRequestGeneral>(username, boost::none);

    // Get the authorization session that will be used when running the command.
    auto authzSessionForCommand = AuthorizationSession::get(_opCtx->getClient());
    ASSERT_OK(createUser(username, {{"readWrite", "test"}, {"dbAdmin", "test"}}));
    ASSERT_OK(authzSessionForCommand->addAndAuthorizeUser(
        _opCtx.get(), usernameTestRequest->clone(), boost::none));


    // Assume privileges for the database to allow killAnyCursor action.
    PrivilegeVector privileges;
    auth::generateUniversalPrivileges(&privileges, boost::none /* tenantId */);
    auto user = authzSessionForCommand->getAuthenticatedUser();
    ASSERT(user != boost::none);
    (*user)->addPrivileges(privileges);
    ASSERT_TRUE(authzSessionForCommand->isAuthorizedForActionsOnResource(
        CommandHelpers::resourcePatternForNamespace(testFooNss), ActionType::killAnyCursor));

    // Get the cursor manager and register a cursor. We need to detach it from the operation context
    // to allow killCursors to reattach it later during execution.
    CursorManager* _cursorManager = CursorManager::get(_opCtx.get());
    auto exec = makeFakePlanExecutor(_opCtx.get(), testFooNss);
    exec->saveState();
    exec->detachFromOperationContext();

    auto cursorPin = _cursorManager->registerCursor(
        _opCtx.get(),
        {
            std::move(exec),
            testFooNss,
            username,
            APIParameters(),
            _opCtx->getWriteConcern(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            BSONObj(),
            PrivilegeVector(),
        });
    const auto cursorId = cursorPin->cursorid();
    cursorPin.release();

    auto authCheck = [&](const ClientCursor& cc) {
        uassertStatusOK(auth::checkAuthForKillCursors(
            AuthorizationSession::get(_opCtx->getClient()), cc.nss(), cc.getAuthenticatedUser()));
    };
    ASSERT_OK(_cursorManager->killCursorWithAuthCheck(_opCtx.get(), cursorId, authCheck));
}

TEST_F(AuthorizationSessionTest, CheckAuthorizationForKillCursorsUnauthorizedUser) {
    authzManager->setAuthEnabled(true);
    UserName usernameUnauth("fakeSpencer", "admin");

    // Get the cursor manager and register a cursor. We need to detach it from the operation context
    // to allow killCursors to reattach it later during execution.
    CursorManager* _cursorManager = CursorManager::get(_opCtx.get());
    auto exec = makeFakePlanExecutor(_opCtx.get(), testFooNss);
    exec->saveState();
    exec->detachFromOperationContext();

    auto cursorPin = _cursorManager->registerCursor(
        _opCtx.get(),
        {
            std::move(exec),
            testFooNss,
            usernameUnauth,
            APIParameters(),
            _opCtx->getWriteConcern(),
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            BSONObj(),
            PrivilegeVector(),
        });
    const auto cursorId = cursorPin->cursorid();
    cursorPin.release();

    auto authCheck = [&](const ClientCursor& cc) {
        uassertStatusOK(auth::checkAuthForKillCursors(
            AuthorizationSession::get(_opCtx->getClient()), cc.nss(), cc.getAuthenticatedUser()));
    };
    ASSERT_THROWS_CODE(_cursorManager->killCursorWithAuthCheck(_opCtx.get(), cursorId, authCheck),
                       DBException,
                       ErrorCodes::Unauthorized);
}

class SystemBucketsTest : public AuthorizationSessionTest {
protected:
    static const DatabaseName sb_db_test;
    static const DatabaseName sb_db_other;

    static const ResourcePattern testMissingSystemBucketResource;
    static const ResourcePattern otherMissingSystemBucketResource;
    static const ResourcePattern otherDbMissingSystemBucketResource;

    static const ResourcePattern testSystemBucketResource;
    static const ResourcePattern otherSystemBucketResource;
    static const ResourcePattern otherDbSystemBucketResource;

    static const ResourcePattern testBucketResource;
    static const ResourcePattern otherBucketResource;
    static const ResourcePattern otherDbBucketResource;

    static const ResourcePattern sbCollTestInAnyDB;
};

const DatabaseName SystemBucketsTest::sb_db_test =
    DatabaseName::createDatabaseName_forTest(boost::none, "sb_db_test"_sd);
const DatabaseName SystemBucketsTest::sb_db_other =
    DatabaseName::createDatabaseName_forTest(boost::none, "sb_db_other"_sd);

const ResourcePattern SystemBucketsTest::testMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_test.sb_coll_test")));
const ResourcePattern SystemBucketsTest::otherMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_test.sb_coll_other")));
const ResourcePattern SystemBucketsTest::otherDbMissingSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_other.sb_coll_test")));

const ResourcePattern SystemBucketsTest::testSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_test.system.buckets.sb_coll_test")));
const ResourcePattern SystemBucketsTest::otherSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_test.system.buckets.sb_coll_other")));
const ResourcePattern SystemBucketsTest::otherDbSystemBucketResource(
    ResourcePattern::forExactNamespace(
        NamespaceString::createNamespaceString_forTest("sb_db_other.system.buckets.sb_coll_test")));

const ResourcePattern SystemBucketsTest::testBucketResource(
    ResourcePattern::forExactSystemBucketsCollection(NamespaceString::createNamespaceString_forTest(
        boost::none /* tenantId */, "sb_db_test"_sd, "sb_coll_test"_sd)));
const ResourcePattern SystemBucketsTest::otherBucketResource(
    ResourcePattern::forExactSystemBucketsCollection(NamespaceString::createNamespaceString_forTest(
        boost::none /* tenantId */, "sb_db_test"_sd, "sb_coll_other"_sd)));
const ResourcePattern SystemBucketsTest::otherDbBucketResource(
    ResourcePattern::forExactSystemBucketsCollection(NamespaceString::createNamespaceString_forTest(
        boost::none /* tenantId */, "sb_db_other"_sd, "sb_coll_test"_sd)));

const ResourcePattern SystemBucketsTest::sbCollTestInAnyDB(
    ResourcePattern::forAnySystemBucketsInAnyDatabase(boost::none, "sb_coll_test"_sd));

TEST_F(SystemBucketsTest, CheckExactSystemBucketsCollection) {
    // If we have a system_buckets exact priv
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find), testDB);

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
        Privilege(ResourcePattern::forAnySystemBuckets(boost::none), ActionType::find), testDB);

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
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test), ActionType::find),
        testDB);

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
    authzSession->assumePrivilegesForDB(Privilege(sbCollTestInAnyDB, ActionType::find), testDB);


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
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));

    // If we have any buckets in a database privilege, we have actions on that database and all
    // system.buckets collections it contains
    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test), ActionType::find),
        testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test)));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));

    // If we have a privilege on any systems buckets in any db, we have actions on all databases and
    // system.buckets.<coll> they contain
    authzSession->assumePrivilegesForDB(Privilege(sbCollTestInAnyDB, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnResource(testSystemBucketResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(
        ResourcePattern::forDatabaseName(sb_db_test)));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyNormalResource));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnResource(kAnyResource));
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

    // If we have clusterMonitor, make sure the following actions are authorized o any system
    // bucket: collStats, dbStats, getDatabaseVersion, getShardVersion and indexStats.
    authzSession->assumePrivilegesForBuiltinRole(RoleName("clusterMonitor", "admin"));

    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(kAnySystemBucketResource,
                                                               ActionType::collStats));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(kAnySystemBucketResource,
                                                               ActionType::dbStats));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(kAnySystemBucketResource,
                                                               ActionType::getDatabaseVersion));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(kAnySystemBucketResource,
                                                               ActionType::getShardVersion));
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(kAnySystemBucketResource,
                                                               ActionType::indexStats));
}

TEST_F(SystemBucketsTest, CanCheckIfHasAnyPrivilegeInResourceDBForSystemBuckets) {
    authzSession->assumePrivilegesForDB(Privilege(testBucketResource, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBucketsInDatabase(sb_db_test), ActionType::find),
        testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_FALSE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(Privilege(sbCollTestInAnyDB, ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));

    authzSession->assumePrivilegesForDB(
        Privilege(ResourcePattern::forAnySystemBuckets(boost::none), ActionType::find), testDB);
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_test));
    ASSERT_TRUE(authzSession->isAuthorizedForAnyActionOnAnyResourceInDB(sb_db_other));
}

TEST_F(AuthorizationSessionTest, InternalSystemClientsBypassValidateRestrictions) {
    // set up a direct client without transport session
    auto client = getServiceContext()->getService()->makeClient("directClient");
    // set Client user to be the internal __system user.
    authzSession->grantInternalAuthorization();
    auto opCtx = client->makeOperationContext();

    ASSERT(authzSession->getAuthenticatedUser().has_value());
    UserHandle currentUser = authzSession->getAuthenticatedUser().value();
    ASSERT(currentUser.isValid());
    ASSERT(!currentUser->isInvalidated());

    // invalidate the __system user to force the next request to validate restrictions
    currentUser->invalidate();
    ASSERT(currentUser.isValid());
    ASSERT(currentUser->isInvalidated());

    // should not fail even though client does not have a transport session
    authzSession->startRequest(opCtx.get());
    ASSERT_OK(currentUser->validateRestrictions(opCtx.get()));
}

TEST_F(AuthorizationSessionTest, ClusterActionsTestInternal) {
    authzSession->grantInternalAuthorization();
    ASSERT_TRUE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, boost::none));
    ASSERT_TRUE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, kTenantId1));
    ASSERT_TRUE(authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS,
                                                           boost::none));
    ASSERT_TRUE(
        authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS, kTenantId1));
    authzSession->logoutAllDatabases("Test finished");
}

TEST_F(AuthorizationSessionTest, ClusterActionsTestAdmin) {
    authzManager->setAuthEnabled(true);
    authzSession->assumePrivilegesForBuiltinRole(RoleName{"root", "admin"});
    ASSERT_FALSE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, boost::none));
    ASSERT_FALSE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, kTenantId1));
    ASSERT_TRUE(authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS,
                                                           boost::none));
    ASSERT_TRUE(
        authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS, kTenantId1));
    authzSession->logoutAllDatabases("Test finished");
}

TEST_F(AuthorizationSessionTest, ClusterActionsTestUser) {
    authzManager->setAuthEnabled(true);
    authzSession->assumePrivilegesForBuiltinRole(RoleName{"read", "admin"});
    ASSERT_FALSE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, boost::none));
    ASSERT_FALSE(
        authzSession->isAuthorizedForClusterAction(ActionType::advanceClusterTime, kTenantId1));
    ASSERT_FALSE(authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS,
                                                            boost::none));
    ASSERT_FALSE(
        authzSession->isAuthorizedForClusterAction(ActionType::bypassDefaultMaxTimeMS, kTenantId1));
    authzSession->logoutAllDatabases("Test finished");
}

// Test agg stage that doesn't use authz checks and doesn't use opt-out
class TestDocumentSourceNoPrivsWithAuthzChecks : public DocumentSource {
public:
    static constexpr StringData kStageName = "$testNoPrivsWithAuthzChecks"_sd;

    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(spec.fieldName());
        }

        LiteParsed(std::string parseTimeName)
            : LiteParsedDocumentSource(std::move(parseTimeName)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return {};
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {};
        }
    };

    TestDocumentSourceNoPrivsWithAuthzChecks(auto stageName,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(stageName, expCtx) {}

    GetNextResult doGetNext() {
        return GetNextResult::makeEOF();
    }

    const char* getSourceName() const override {
        return kStageName.data();
    }

    StageConstraints constraints(PipelineSplitState pipStage) const final {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed,
                                ChangeStreamRequirement::kAllowlist);
    }
    Id getId() const override {
        return 0;
    }
    void addVariableRefs(std::set<Variables::Id>* refs) const override {}
    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return DistributedPlanLogic{};
    }
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const override {
        return Value{};
    }

    static auto createFromBson(BSONElement elem,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return boost::intrusive_ptr<TestDocumentSourceNoPrivsWithAuthzChecks>{
            new TestDocumentSourceNoPrivsWithAuthzChecks("$testNoPrivsWithAuthzChecks", expCtx)};
    }
};

REGISTER_DOCUMENT_SOURCE(testNoPrivsWithAuthzChecks,
                         TestDocumentSourceNoPrivsWithAuthzChecks::LiteParsed::parse,
                         TestDocumentSourceNoPrivsWithAuthzChecks::createFromBson,
                         AllowedWithApiStrict::kAlways);

// Test agg stage that doesn't use authz checks and uses opt-out
class TestDocumentSourceNoPrivsWithAuthzChecksOptOut
    : public TestDocumentSourceNoPrivsWithAuthzChecks {
public:
    static constexpr StringData kStageName = "$testNoPrivsWithAuthzChecksOptOut"_sd;

    const char* getSourceName() const override {
        return kStageName.data();
    }

    class LiteParsedOptOut : public TestDocumentSourceNoPrivsWithAuthzChecks::LiteParsed {
    public:
        static std::unique_ptr<LiteParsedOptOut> parse(const NamespaceString& nss,
                                                       const BSONElement& spec,
                                                       const LiteParserOptions& options) {
            return std::make_unique<LiteParsedOptOut>(spec.fieldName());
        }

        LiteParsedOptOut(std::string parseTimeName)
            : TestDocumentSourceNoPrivsWithAuthzChecks::LiteParsed(std::move(parseTimeName)) {}

        // Opt-out
        bool requiresAuthzChecks() const override {
            return false;
        }
    };

    TestDocumentSourceNoPrivsWithAuthzChecksOptOut(
        auto stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : TestDocumentSourceNoPrivsWithAuthzChecks(stageName, expCtx) {}

    static auto createFromBson(BSONElement elem,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return boost::intrusive_ptr<TestDocumentSourceNoPrivsWithAuthzChecksOptOut>{
            new TestDocumentSourceNoPrivsWithAuthzChecksOptOut("$testNoPrivsWithAuthzChecksOptOut",
                                                               expCtx)};
    }
};

REGISTER_DOCUMENT_SOURCE(testNoPrivsWithAuthzChecksOptOut,
                         TestDocumentSourceNoPrivsWithAuthzChecksOptOut::LiteParsedOptOut::parse,
                         TestDocumentSourceNoPrivsWithAuthzChecksOptOut::createFromBson,
                         AllowedWithApiStrict::kAlways);

// Test agg stage that uses authz checks (no need for opt out)
class TestDocumentSourceWithPrivs : public TestDocumentSourceNoPrivsWithAuthzChecks {
public:
    static constexpr StringData kStageName = "$testWithPrivs"_sd;

    const char* getSourceName() const override {
        return kStageName.data();
    }

    class LiteParsedWithPrivs : public TestDocumentSourceWithPrivs::LiteParsed {
    public:
        static std::unique_ptr<LiteParsedWithPrivs> parse(const NamespaceString& nss,
                                                          const BSONElement& spec,
                                                          const LiteParserOptions& options) {
            return std::make_unique<LiteParsedWithPrivs>(spec.fieldName(), nss);
        }

        // Includes authz checks
        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {Privilege(ResourcePattern::forExactNamespace(_namespace), ActionType::find)};
        }

        LiteParsedWithPrivs(std::string parseTimeName, NamespaceString nss)
            : TestDocumentSourceNoPrivsWithAuthzChecks::LiteParsed(std::move(parseTimeName)),
              _namespace(std::move(nss)) {}

    private:
        NamespaceString _namespace;
    };

    TestDocumentSourceWithPrivs(auto stageName,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : TestDocumentSourceNoPrivsWithAuthzChecks(stageName, expCtx) {}

    static auto createFromBson(BSONElement elem,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return boost::intrusive_ptr<TestDocumentSourceWithPrivs>{
            new TestDocumentSourceWithPrivs("$testWithPrivs", expCtx)};
    }
};

REGISTER_DOCUMENT_SOURCE(testWithPrivs,
                         TestDocumentSourceWithPrivs::LiteParsedWithPrivs::parse,
                         TestDocumentSourceWithPrivs::createFromBson,
                         AllowedWithApiStrict::kAlways);

//  Multitenancy disabled
DEATH_TEST_F(
    AuthorizationSessionTest,
    AggStageFailsRequiresAuthzChecksWithNoPrivilegesAndNoOptOutMultitenancyDisabled,
    "Must specify authorization checks for this stage: $testNoPrivsWithAuthzChecks or manually "
    "opt out by overriding requiresAuthzChecks to false") {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagMandatoryAuthzChecks",
                                                               true};

    auto nss = testFooNss;
    auto rsrc = ResourcePattern::forExactNamespace(nss);

    // 1. Stage does not use authz checks, and developer did not opt out of authz checks -> FAIL
    BSONArray pipeline_test_one = BSON_ARRAY(BSON("$testNoPrivsWithAuthzChecks" << nss.coll()));
    auto aggReq_test_one = buildAggReq(nss, pipeline_test_one);
    auto priv = auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq_test_one, false);
}

//  Multitenancy enabled
DEATH_TEST_F(
    AuthorizationSessionTest,
    AggStageFailsRequiresAuthzChecksWithNoPrivilegesAndNoOptOutMultitenancyEnabled,
    "Must specify authorization checks for this stage: $testNoPrivsWithAuthzChecks or manually "
    "opt out by overriding requiresAuthzChecks to false") {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagMandatoryAuthzChecks",
                                                               true};

    auto nss = testTenant1FooNss;
    auto rsrc = ResourcePattern::forExactNamespace(nss);

    // 1. Stage does not use authz checks, and developer did not opt out of authz checks -> FAIL
    BSONArray pipeline_test_one = BSON_ARRAY(BSON("$testNoPrivsWithAuthzChecks" << nss.coll()));
    auto aggReq_test_one = buildAggReq(nss, pipeline_test_one);
    auto priv = auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq_test_one, false);
}

TEST_F(AuthorizationSessionTest, AggStagePassesRequiresAuthzChecksWithPrivilegesOrOptOut) {
    for (auto multitenancy : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancy);
        RAIIServerParameterControllerForTest featureFlagController{
            "featureFlagMandatoryAuthzChecks", true};

        auto nss = multitenancy ? testTenant1FooNss : testFooNss;
        auto rsrc = ResourcePattern::forExactNamespace(nss);

        // 2. Stage does not use authz checks, and developer opted out of authz checks -> SUCCESS
        BSONArray pipeline_test_two =
            BSON_ARRAY(BSON("$testNoPrivsWithAuthzChecksOptOut" << nss.coll()));
        auto aggReq_test_two = buildAggReq(nss, pipeline_test_two);
        ASSERT_OK(auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq_test_two, false));

        // 3. Stage uses authz checks, and authz checks were implemented -> SUCCESS
        BSONArray pipeline_test_three = BSON_ARRAY(BSON("$testWithPrivs" << nss.coll()));
        auto aggReq_test_three = buildAggReq(nss, pipeline_test_three);
        ASSERT_OK(
            auth::getPrivilegesForAggregate(authzSession.get(), nss, aggReq_test_three, false));
    }
}
}  // namespace
}  // namespace mongo
