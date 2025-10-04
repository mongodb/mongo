/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/op_msg_fuzzer_router_fixture.h"

#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
    _serviceContext->getService(ClusterRole::RouterServer)
        ->setServiceEntryPoint(std::make_unique<ServiceEntryPointRouterRole>());

    _serviceContext->setPeriodicRunner(makePeriodicRunner(_serviceContext));

    _routerStrand = ClientStrand::make(
        _serviceContext->getService(ClusterRole::RouterServer)->makeClient("routerTest", _session));
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
