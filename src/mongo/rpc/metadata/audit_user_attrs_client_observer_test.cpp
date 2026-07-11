// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata/audit_user_attrs_client_observer.h"

#include "mongo/db/auth/authorization_session_test_fixture.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::audit {

namespace {
using namespace std::literals::string_view_literals;

class AuditDecorationsTest : public AuthorizationSessionTestFixture {
protected:
    explicit AuditDecorationsTest(Options options = Options{})
        : AuthorizationSessionTestFixture(std::move(options)) {}
};

const UserName kUser1Test("user1"sv, "test"sv);
const std::unique_ptr<UserRequest> kUser1TestRequest =
    std::make_unique<UserRequestGeneral>(kUser1Test, boost::none);


TEST_F(AuditDecorationsTest, basicAuditUserAttrsCheck) {
    ASSERT_OK(createUser(kUser1Test, {}));

    auto newClient = getService()->makeClient("client1");
    ASSERT_OK(AuthorizationSession::get(newClient.get())
                  ->addAndAuthorizeUser(_opCtx.get(), kUser1TestRequest->clone(), boost::none));

    auto opCtx2 = newClient->makeOperationContext();
    auto auditAttrs = rpc::AuditUserAttrs::get(opCtx2.get());

    ASSERT(auditAttrs);
    ASSERT_EQ(auditAttrs->getUser().getUser(), "user1");
    ASSERT_EQ(auditAttrs->getRoles().size(), 0);
}

}  // namespace

}  // namespace mongo::audit
