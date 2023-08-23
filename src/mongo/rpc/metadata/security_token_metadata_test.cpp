/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_test.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {
namespace test {
namespace {

constexpr auto kPingFieldName = "ping"_sd;

std::string makeSecurityToken(const UserName& userName) {
    using VTS = auth::ValidatedTenancyScope;
    return VTS(userName, "secret"_sd, VTS::TokenForTestingTag{}).getOriginalToken().toString();
}

class SecurityTokenMetadataTest : public ServiceContextTest {
protected:
    void setUp() final {
        client = getServiceContext()->makeClient("test");
    }

    ServiceContext::UniqueClient client;
};

TEST_F(SecurityTokenMetadataTest, SecurityTokenSingletenancy) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", false);

    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = makeSecurityToken(UserName("user", "admin", TenantId(OID::gen())));

    auto msgBytes = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody};
    ASSERT_THROWS_CODE_AND_WHAT(msgBytes.parse(),
                                DBException,
                                ErrorCodes::Unauthorized,
                                "Unsupported Security Token provided");
}

TEST_F(SecurityTokenMetadataTest, SecurityTokenNotAccepted) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", false);

    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = makeSecurityToken(UserName("user", "admin", TenantId(OID::gen())));

    auto msgBytes = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody};
    ASSERT_THROWS_CODE_AND_WHAT(
        msgBytes.parse(),
        DBException,
        ErrorCodes::Unauthorized,
        "Signed authentication tokens are not accepted without feature flag opt-in");
}

TEST_F(SecurityTokenMetadataTest, SecurityTokenTestTokensNotAvailable) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);

    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = makeSecurityToken(UserName("user", "admin", TenantId(OID::gen())));

    auto msgBytes = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody};
    ASSERT_THROWS_CODE_AND_WHAT(
        msgBytes.parse(),
        DBException,
        ErrorCodes::OperationFailed,
        "Unable to validate test tokens when testOnlyValidatedTenancyScopeKey is not provided");
}

TEST_F(SecurityTokenMetadataTest, BasicSuccess) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest securityTokenController("featureFlagSecurityToken", true);
    RAIIServerParameterControllerForTest secretController("testOnlyValidatedTenancyScopeKey",
                                                          "secret");

    const auto kTenantId = TenantId(OID::gen());
    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = makeSecurityToken(UserName("user", "admin", kTenantId));

    auto msg = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody}.parse();
    ASSERT_BSONOBJ_EQ(msg.body, kPingBody);
    ASSERT_EQ(msg.sequences.size(), 0u);
    ASSERT_TRUE(msg.validatedTenancyScope != boost::none);
    ASSERT_EQ(msg.validatedTenancyScope->getOriginalToken(), kTokenBody);
    ASSERT_EQ(msg.validatedTenancyScope->tenantId(), kTenantId);

    auto opCtx = makeOperationContext();
    ASSERT(auth::ValidatedTenancyScope::get(opCtx.get()) == boost::none);

    auth::ValidatedTenancyScope::set(opCtx.get(), msg.validatedTenancyScope);
    auto token = auth::ValidatedTenancyScope::get(opCtx.get());
    ASSERT(token != boost::none);

    ASSERT_TRUE(token->hasAuthenticatedUser());
    auto authedUser = token->authenticatedUser();
    ASSERT_EQ(authedUser.getUser(), "user");
    ASSERT_EQ(authedUser.getDB(), "admin");
    ASSERT_TRUE(authedUser.getTenant() != boost::none);
    ASSERT_EQ(authedUser.getTenant().value(), kTenantId);
}

}  // namespace
}  // namespace test
}  // namespace rpc
}  // namespace mongo
