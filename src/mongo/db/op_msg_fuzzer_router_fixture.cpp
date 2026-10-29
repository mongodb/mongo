// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_msg_fuzzer_router_fixture.h"

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_client_handle_router.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/message.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/version/releases.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

// This must be called before creating any new threads that may access `AuthorizationManager` to
// avoid a data-race.
void OpMsgFuzzerRouterFixture::_setAuthorizationManager() {
    auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
    AuthorizationManager::set(
        _serviceContext->getService(),
        globalAuthzManagerFactory->createRouter(_serviceContext->getService()));
    AuthorizationManager::get(_serviceContext->getService())->setAuthEnabled(true);
}

OpMsgFuzzerRouterFixture::OpMsgFuzzerRouterFixture(bool skipGlobalInitializers) {
    if (!skipGlobalInitializers) {
        auto ret = runGlobalInitializers(std::vector<std::string>{});
        invariant(ret);
    }

    serverGlobalParams.clusterRole = ClusterRole::RouterServer;

    setGlobalServiceContext(ServiceContext::make());
    _session = _transportLayer.createSession();

    _serviceContext = getGlobalServiceContext();
    _setAuthorizationManager();
    _serviceContext->getService()->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointRouterRole>());

    _serviceContext->setPeriodicRunner(makePeriodicRunner(_serviceContext));

    _routerStrand =
        ClientStrand::make(_serviceContext->getService()->makeClient("routerTest", _session));
}

int OpMsgFuzzerRouterFixture::testOneInput(const char* Data, size_t Size) {
    if (Size < sizeof(MSGHEADER::Value)) {
        return 0;
    }

    ClientStrand::Guard clientGuard = _routerStrand->bind();

    auto opCtx = _serviceContext->makeOperationContext(clientGuard.get());

    int new_size = Size + sizeof(int);
    auto sb = SharedBuffer::allocate(new_size);
    memcpy(sb.get(), &new_size, sizeof(int));
    memcpy(sb.get() + sizeof(int), Data, Size);
    Message msg(std::move(sb));

    try {
        _serviceContext->getService()
            ->getServiceEntryPoint()
            ->handleRequest(opCtx.get(), msg, _serviceContext->getFastClockSource()->now())
            .get();
    } catch (const AssertionException&) {
        // We need to catch exceptions caused by invalid inputs
    }
    return 0;
}

}  // namespace mongo
