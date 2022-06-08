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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/rpc/op_msg_test.h"

#include "mongo/platform/basic.h"

#include "mongo/base/static_assert.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/security_token.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

namespace mongo {

class AuthorizationSessionImplTestHelper {
public:
    /**
     * Synthesize a user with the useTenant privilege and add them to the authorization session.
     */
    static void grantUseTenant(Client& client) {
        User user(UserName("useTenant", "admin"));
        user.setPrivileges(
            {Privilege(ResourcePattern::forClusterResource(), ActionType::useTenant)});
        auto* as = dynamic_cast<AuthorizationSessionImpl*>(AuthorizationSession::get(client));
        if (as->_authenticatedUser != boost::none) {
            as->logoutAllDatabases(&client, "AuthorizationSessionImplTestHelper"_sd);
        }
        as->_authenticatedUser = std::move(user);
        as->_authenticationMode = AuthorizationSession::AuthenticationMode::kConnection;
        as->_updateInternalAuthorizationState();
    }
};

class ValidatedTenantIdTestFixture : public mongo::ScopedGlobalServiceContextForTest,
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

    BSONObj makeSecurityToken(const UserName& userName) {
        constexpr auto authUserFieldName = auth::SecurityToken::kAuthenticatedUserFieldName;
        auto authUser = userName.toBSON(true /* serialize token */);
        ASSERT_EQ(authUser["tenant"_sd].type(), jstOID);
        return auth::signSecurityToken(BSON(authUserFieldName << authUser));
    }

    ServiceContext::UniqueClient client;
};

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportOffWithoutTenantOK) {
    gMultitenancySupport = false;
    auto body = fromjson("{$db: 'foo'}");
    OpMsg msg({body});

    ValidatedTenantId validatedTenant(msg, *(client.get()));
    ASSERT_TRUE(validatedTenant.tenantId() == boost::none);
}

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportWithTenantOK) {
    gMultitenancySupport = true;

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);
    OpMsg msg({body});

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    ValidatedTenantId validatedTenant(msg, *(client.get()));
    ASSERT(validatedTenant.tenantId() == TenantId(kOid));
}

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportWithSecurityTokenOK) {
    gMultitenancySupport = true;

    const auto kTenantId = TenantId(OID::gen());
    auto body = BSON("ping" << 1);
    auto token = makeSecurityToken(UserName("user", "admin", kTenantId));
    OpMsg msg({body, token});

    ValidatedTenantId validatedTenant(msg, *(client.get()));
    ASSERT(validatedTenant.tenantId() == kTenantId);
}

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportOffWithTenantNOK) {
    gMultitenancySupport = false;

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);
    OpMsg msg({body});

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    ASSERT_THROWS_CODE(
        ValidatedTenantId(msg, *(client.get())), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportWithTenantNOK) {
    gMultitenancySupport = true;

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);
    OpMsg msg({body});

    ASSERT_THROWS_CODE(
        ValidatedTenantId(msg, *(client.get())), DBException, ErrorCodes::Unauthorized);
}

// TODO SERVER-66822: Re-enable this test case.
// TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportWithoutTenantAndSecurityTokenNOK) {
//     gMultitenancySupport = true;

//     auto body = BSON("ping" << 1);
//     OpMsg msg({body});

//     AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
//     ASSERT_THROWS_CODE(
//         ValidatedTenantId(msg, *(client.get())), DBException, ErrorCodes::Unauthorized);
// }

TEST_F(ValidatedTenantIdTestFixture, MultitenancySupportWithTenantAndSecurityTokenNOK) {
    gMultitenancySupport = true;

    auto kOid = OID::gen();
    auto body = BSON("ping" << 1 << "$tenant" << kOid);
    auto token = makeSecurityToken(UserName("user", "admin", TenantId(kOid)));
    OpMsg msg({body, token});

    AuthorizationSessionImplTestHelper::grantUseTenant(*(client.get()));
    ASSERT_THROWS_CODE(ValidatedTenantId(msg, *(client.get())), DBException, 6545800);
}

}  // namespace mongo
