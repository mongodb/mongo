// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/service_context_test_fixture.h"

#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/wire_version.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

#include <utility>

namespace mongo {

ServiceContext::UniqueServiceContext makeServiceContext(bool shouldSetupTL = false) {
    {
        // Reset the global clock source
        ClockSourceMock clkSource;
        clkSource.reset();
    }

    return ServiceContext::make();
}

ScopedGlobalServiceContextForTest::ScopedGlobalServiceContextForTest()
    : ScopedGlobalServiceContextForTest(makeServiceContext(), false) {}

ScopedGlobalServiceContextForTest::ScopedGlobalServiceContextForTest(bool shouldSetupTL)
    : ScopedGlobalServiceContextForTest(makeServiceContext(), shouldSetupTL) {}

ScopedGlobalServiceContextForTest::ScopedGlobalServiceContextForTest(
    ServiceContext::UniqueServiceContext serviceContext, bool shouldSetupTL) {
    if (shouldSetupTL) {
        auto tl = transport::TransportLayerManagerImpl::makeDefaultEgressTransportLayer();
        uassertStatusOK(tl->setup());
        uassertStatusOK(tl->start());
        serviceContext->setTransportLayerManager(std::move(tl));
    }

    WireSpec::getWireSpec(serviceContext.get()).initialize(WireSpec::Specification{});
    setGlobalServiceContext(std::move(serviceContext));

    auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
    AuthorizationManager::set(getService(), globalAuthzManagerFactory->createShard(getService()));
    auth::AuthorizationBackendInterface::set(
        getService(), globalAuthzManagerFactory->createBackendInterface(getService()));

    query_settings::QuerySettingsService::initializeForTest(getServiceContext());
}

ScopedGlobalServiceContextForTest::~ScopedGlobalServiceContextForTest() {
    // TODO: SERVER-67478 Remove shutdown.
    // Join all task executor and network thread in repl monitor to prevent it from racing with
    // setGlobalServiceContext when they call getGlobalServiceContext.
    ReplicaSetMonitorManager::get()->shutdown();

    setGlobalServiceContext({});
}

ServiceContext* ScopedGlobalServiceContextForTest::getServiceContext() const {
    return getGlobalServiceContext();
}

Service* ScopedGlobalServiceContextForTest::getService() const {
    return getServiceContext()->getService();
}

ServiceContextTest::ServiceContextTest()
    : _scopedServiceContext(std::make_unique<ScopedGlobalServiceContextForTest>(shouldSetupTL)),
      _threadClient(_scopedServiceContext->getService(), nullptr) {}

ServiceContextTest::ServiceContextTest(
    std::unique_ptr<ScopedGlobalServiceContextForTest> scopedServiceContext,
    std::shared_ptr<transport::Session> session)
    : _scopedServiceContext(std::move(scopedServiceContext)),
      _threadClient(_scopedServiceContext->getService(), session) {}

ServiceContextTest::~ServiceContextTest() = default;

ClockSourceMockServiceContextTest::ClockSourceMockServiceContextTest()
    : ServiceContextTest(std::make_unique<ScopedGlobalServiceContextForTest>(
          ServiceContext::make(std::make_unique<ClockSourceMock>(),
                               std::make_unique<ClockSourceMock>(),
                               std::make_unique<TickSourceMock<Milliseconds>>()),
          shouldSetupTL)) {}

SharedClockSourceAdapterServiceContextTest::SharedClockSourceAdapterServiceContextTest(
    std::shared_ptr<ClockSource> clock)
    : ServiceContextTest(std::make_unique<ScopedGlobalServiceContextForTest>(
          ServiceContext::make(std::make_unique<SharedClockSourceAdapter>(clock),
                               std::make_unique<SharedClockSourceAdapter>(clock),
                               std::make_unique<TickSourceMock<>>()),
          shouldSetupTL)) {}

}  // namespace mongo
