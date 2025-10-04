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
    auto sc = getServiceContext();
    // Just pick any service. Giving priority to Shard.
    if (auto srv = sc->getService(ClusterRole::ShardServer))
        return srv;
    if (auto srv = sc->getService(ClusterRole::RouterServer))
        return srv;
    MONGO_UNREACHABLE;
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
