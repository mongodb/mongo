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

#include "mongo/platform/basic.h"

#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/security_token.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DollarTenantDecorationTest : public ScopedGlobalServiceContextForTest, public unittest::Test {
protected:
    void setUp() final {
        auto authzManagerState = std::make_unique<AuthzManagerExternalStateMock>();
        auto authzManager = std::make_unique<AuthorizationManagerImpl>(
            getServiceContext(), std::move(authzManagerState));
        authzManager->setAuthEnabled(true);
        AuthorizationManager::set(getServiceContext(), std::move(authzManager));

        client = getServiceContext()->makeClient("test");
        opCtxPtr = getServiceContext()->makeOperationContext(client.get());
        opCtx = opCtxPtr.get();
    }

    BSONObj makeSecurityToken(const UserName& userName) {
        auto authUser = userName.toBSON(true /* serialize token */);
        ASSERT_EQ(authUser["tenant"_sd].type(), jstOID);

        BSONObjBuilder token;
        token.append("authenticatedUser", authUser);

        auto block =
            SHA256Block::computeHash({ConstDataRange(authUser.objdata(), authUser.objsize())});
        token.appendBinData("sig", block.size(), BinDataGeneral, block.data());

        return token.obj();
    }

    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtxPtr;
    OperationContext* opCtx;
};

TEST_F(DollarTenantDecorationTest, ParseDollarTenantFromRequestSecurityTokenAlreadySet) {
    gMultitenancySupport = true;

    // Ensure the security token is set on the opCtx.
    const auto kOid = OID::gen();
    auto token = makeSecurityToken(UserName("user", "admin", kOid));
    auth::readSecurityTokenMetadata(opCtx, token);
    ASSERT(getActiveTenant(opCtx));
    ASSERT_EQ(*getActiveTenant(opCtx), kOid);

    // TODO SERVER-62406 use the new ActionType for use with $tenant.
    // Grant internal auth so that we're authenticated as the internal __system user.
    AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx);

    // The dollarTenantDecoration should not be set because the security token is already set.
    const auto kOidParameter = OID::gen();
    auto opMsgRequest = OpMsgRequest::fromDBAndBody("test", BSON("$tenant" << kOidParameter));
    ASSERT_THROWS_CODE(
        parseDollarTenantFromRequest(opCtx, opMsgRequest), AssertionException, 6223901);

    // getActiveTenant should still return the tenantId in the security token.
    ASSERT(getActiveTenant(opCtx));
    ASSERT_EQ(*getActiveTenant(opCtx), kOid);
}

TEST_F(DollarTenantDecorationTest, ParseDollarTenantFromRequestNotInternalSecurityUser) {
    gMultitenancySupport = true;
    const auto kOid = OID::gen();

    // We are not authenticated as the internal security user.
    auto opMsgRequest = OpMsgRequest::fromDBAndBody("test", BSON("$tenant" << kOid));
    ASSERT_THROWS_CODE(parseDollarTenantFromRequest(opCtx, opMsgRequest),
                       AssertionException,
                       ErrorCodes::Unauthorized);
    ASSERT(!getActiveTenant(opCtx));
}

TEST_F(DollarTenantDecorationTest, ParseDollarTenantMultitenancySupportDisabled) {
    gMultitenancySupport = false;
    const auto kOid = OID::gen();

    // TODO SERVER-62406 use the new ActionType for use with $tenant.
    // Grant internal auth so that we're authenticated as the internal __system user.
    AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx);

    // TenantId is passed as the '$tenant' parameter. "multitenancySupport" is disabled, so we
    // should throw when attempting to set this tenantId on the opCtx.
    auto opMsgRequestParameter = OpMsgRequest::fromDBAndBody("test", BSON("$tenant" << kOid));
    ASSERT_THROWS_CODE(parseDollarTenantFromRequest(opCtx, opMsgRequestParameter),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
    ASSERT(!getActiveTenant(opCtx));
}

TEST_F(DollarTenantDecorationTest, ParseDollarTenantFromRequestSuccess) {
    gMultitenancySupport = true;
    const auto kOid = OID::gen();

    // TODO SERVER-62406 use the new ActionType for use with $tenant.
    // Grant internal auth so that we're authenticated as the internal __system user.
    AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx);

    // The tenantId should be successfully set because "multitenancySupport" is enabled and we're
    // authenticated as the internal __system user.
    auto opMsgRequest = OpMsgRequest::fromDBAndBody("test", BSON("$tenant" << kOid));
    parseDollarTenantFromRequest(opCtx, opMsgRequest);

    auto tenantId = getActiveTenant(opCtx);
    ASSERT(tenantId);
    ASSERT_EQ(*tenantId, kOid);
}

}  // namespace
}  // namespace mongo
