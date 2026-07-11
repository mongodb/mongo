// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace rpc {
namespace test {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kPingFieldName = "ping"sv;

std::string makeSecurityToken(const UserName& userName) {
    return std::string{auth::ValidatedTenancyScopeFactory::create(
                           userName,
                           "secret"sv,
                           auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                           auth::ValidatedTenancyScopeFactory::TokenForTestingTag{})
                           .getOriginalToken()};
}

class SecurityTokenMetadataTest : public ServiceContextTest {
protected:
    void setUp() final {
        client = getServiceContext()->getService()->makeClient("test");
    }

    ServiceContext::UniqueClient client;
};

TEST_F(SecurityTokenMetadataTest, SecurityTokenSingletenancy) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", false);

    const auto kPingBody = BSON(kPingFieldName << 1);
    const auto kTokenBody = makeSecurityToken(UserName("user", "admin", TenantId(OID::gen())));

    auto msgBytes = OpMsgBytes{0, kBodySection, kPingBody, kSecurityTokenSection, kTokenBody};
    ASSERT_THROWS_CODE_AND_WHAT(msgBytes.parse(),
                                DBException,
                                ErrorCodes::Unauthorized,
                                "Unsupported Security Token provided");
}

TEST_F(SecurityTokenMetadataTest, SecurityTokenNotAccepted) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);
    unittest::ServerParameterGuard securityTokenController("featureFlagSecurityToken", false);

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
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);
    unittest::ServerParameterGuard securityTokenController("featureFlagSecurityToken", true);

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
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);
    unittest::ServerParameterGuard securityTokenController("featureFlagSecurityToken", true);
    unittest::ServerParameterGuard secretController("testOnlyValidatedTenancyScopeKey", "secret");

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
    ASSERT_TRUE(authedUser.tenantId() != boost::none);
    ASSERT_EQ(authedUser.tenantId().value(), kTenantId);
}

}  // namespace
}  // namespace test
}  // namespace rpc
}  // namespace mongo
