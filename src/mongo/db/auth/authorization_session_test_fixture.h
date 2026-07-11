// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

class AuthorizationSessionTestFixture : public ServiceContextMongoDTest {
public:
    void setUp() override;

    void tearDown() override {
        authzSession->logoutAllDatabases("Ending AuthorizationSessionTest");
        ServiceContextMongoDTest::tearDown();
        gMultitenancySupport = false;
    }

    Status createUser(const UserName& username, const std::vector<RoleName>& roles);

    void assertLogout(const ResourcePattern& resource, ActionType action);
    void assertExpired(const ResourcePattern& resource, ActionType action);
    void assertActive(const ResourcePattern& resource, ActionType action);
    void assertSecurityToken(const ResourcePattern& resource, ActionType action);
    void assertNotAuthorized(const ResourcePattern& resource, ActionType action);

    static boost::optional<auth::ValidatedTenancyScope> makeVTS(const NamespaceString& nss) {
        if (auto tenantId = nss.tenantId()) {
            return auth::ValidatedTenancyScopeFactory::create(
                *tenantId,
                auth::ValidatedTenancyScope::TenantProtocol::kDefault,
                auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
        }
        return boost::none;
    }

    AggregateCommandRequest buildAggReq(const NamespaceString& nss, const BSONArray& pipeline);

    AggregateCommandRequest buildAggReq(const NamespaceString& nss,
                                        const BSONArray& pipeline,
                                        bool bypassDocValidation);

private:
    static Options createServiceContextOptions(Options options) {
        options = options.setAuthObjects(true);
        return options.useMockClock(true);
    }

protected:
    explicit AuthorizationSessionTestFixture(Options options = Options{})
        : ServiceContextMongoDTest(createServiceContextOptions(std::move(options))) {}

    ClockSourceMock* clockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

protected:
    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> _session;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
    AuthzSessionExternalStateMock* sessionState;
    AuthorizationManager* authzManager;
    auth::AuthorizationBackendMock* backendMock;
    std::unique_ptr<AuthorizationSessionForTest> authzSession;
    BSONObj credentials;
};
}  // namespace mongo
