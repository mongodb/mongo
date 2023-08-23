/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <memory>
#include <set>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class AuthorizationSessionImplTestHelper {
public:
    /**
     * Synthesize a user with the useTenant privilege and add them to the authorization session.
     */
    static void grantUseTenant(Client& client) {
        User user(UserRequest(UserName("useTenant"_sd, "admin"_sd), boost::none));
        user.setPrivileges(
            {Privilege(ResourcePattern::forClusterResource(boost::none), ActionType::useTenant)});
        auto* as = dynamic_cast<AuthorizationSessionImpl*>(AuthorizationSession::get(client));
        if (as->_authenticatedUser != boost::none) {
            as->logoutAllDatabases(&client, "AuthorizationSessionImplTestHelper"_sd);
        }
        as->_authenticatedUser = std::move(user);
        as->_authenticationMode = AuthorizationSession::AuthenticationMode::kConnection;
        as->_updateInternalAuthorizationState();
    }
};

namespace auth {
namespace {

class ValidatedTenancyScopeTestFixture : public mongo::ScopedGlobalServiceContextForTest,
                                         public unittest::Test {
protected:
    void setUp() final {
        auto authzManagerState = std::make_unique<AuthzManagerExternalStateMock>();
        auto authzManager = std::make_unique<AuthorizationManagerImpl>(
            getServiceContext(), std::move(authzManagerState));
        authzManager->setAuthEnabled(true);
        AuthorizationManager::set(getServiceContext(), std::move(authzManager));

        client = getServiceContext()->makeClient("test");
    }

    std::string makeSecurityToken(const UserName& userName) {
        using VTS = auth::ValidatedTenancyScope;
        return VTS(userName, "secret"_sd, VTS::TokenForTestingTag{}).getOriginalToken().toString();
    }

    ServiceContext::UniqueClient client;
};

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportOffWithoutTenantOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);
    auto body = BSON("$db"
                     << "foo");

    auto validated = ValidatedTenancyScope::create(client.get(), body, {});
    ASSERT_TRUE(validated == boost::none);
}

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportWithTenantOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    auto validated = ValidatedTenancyScope::create(client.get(), body, {});
    ASSERT_TRUE(validated != boost::none);
    ASSERT_TRUE(validated->tenantId() == TenantId(kOid));
}

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportWithSecurityTokenOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const TenantId kTenantId(OID::gen());
    auto body = BSON("ping" << 1);
    UserName user("user", "admin", kTenantId);
    auto token = makeSecurityToken(user);

    auto validated = ValidatedTenancyScope::create(client.get(), body, token);
    ASSERT_TRUE(validated != boost::none);
    ASSERT_TRUE(validated->hasTenantId());
    ASSERT_TRUE(validated->tenantId() == kTenantId);
    ASSERT_TRUE(validated->hasAuthenticatedUser());
    ASSERT_TRUE(validated->authenticatedUser() == user);
}

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportOffWithTenantNOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    ASSERT_THROWS_CODE(ValidatedTenancyScope(client.get(), TenantId(kOid)),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_TRUE(ValidatedTenancyScope::create(client.get(), body, {}) == boost::none);
}

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportWithTenantNOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);

    ASSERT_THROWS_CODE(
        ValidatedTenancyScope(client.get(), TenantId(kOid)), DBException, ErrorCodes::Unauthorized);
    ASSERT_THROWS_CODE(ValidatedTenancyScope::create(client.get(), body, ""_sd),
                       DBException,
                       ErrorCodes::Unauthorized);
}

// TODO SERVER-66822: Re-enable this test case.
// TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportWithoutTenantAndSecurityTokenNOK) {
//     RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
//     auto body = BSON("ping" << 1);
//     AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
//     ASSERT_THROWS_CODE(ValidatedTenancyScope::create(client.get(), body, {}), DBException,
//     ErrorCodes::Unauthorized);
// }

TEST_F(ValidatedTenancyScopeTestFixture, MultitenancySupportWithTenantAndSecurityTokenNOK) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);
    UserName user("user", "admin", TenantId(kOid));
    auto token = makeSecurityToken(user);

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    ASSERT_THROWS_CODE(
        ValidatedTenancyScope::create(client.get(), body, token), DBException, 6545800);
}

TEST_F(ValidatedTenancyScopeTestFixture, NoScopeKey) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);

    UserName user("user", "admin", TenantId(OID::gen()));
    auto token = makeSecurityToken(user);
    ASSERT_THROWS_CODE_AND_WHAT(
        ValidatedTenancyScope(client.get(), token),
        DBException,
        ErrorCodes::OperationFailed,
        "Unable to validate test tokens when testOnlyValidatedTenancyScopeKey is not provided");
}

TEST_F(ValidatedTenancyScopeTestFixture, WrongScopeKey) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "password");  // != "secret"

    UserName user("user", "admin", TenantId(OID::gen()));
    auto token = makeSecurityToken(user);
    ASSERT_THROWS_CODE_AND_WHAT(ValidatedTenancyScope(client.get(), token),
                                DBException,
                                ErrorCodes::Unauthorized,
                                "Token signature invalid");
}

}  // namespace
}  // namespace auth
}  // namespace mongo
