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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_test_fixture.h"

#include <memory>

#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/diagnostic_info.h"

namespace mongo {

ScopedGlobalServiceContextForTest::ScopedGlobalServiceContextForTest() {
    {
        // Reset the global clock source
        ClockSourceMock clkSource;
        clkSource.reset();
    }

    auto serviceContext = [] {
        auto serviceContext = ServiceContext::make();
        auto serviceContextPtr = serviceContext.get();
        setGlobalServiceContext(std::move(serviceContext));
        return serviceContextPtr;
    }();

    auto observerRegistry = std::make_unique<OpObserverRegistry>();
    serviceContext->setOpObserver(std::move(observerRegistry));
}

ScopedGlobalServiceContextForTest::~ScopedGlobalServiceContextForTest() {
    // TODO: SERVER-67478 Remove shutdown.
    // Join all task executor and network thread in repl monitor to prevent it from racing with
    // setGlobalServiceContext when they call getGlobalServiceContext.
    ReplicaSetMonitorManager::get()->shutdown();

    setGlobalServiceContext({});
}

ServiceContext* ScopedGlobalServiceContextForTest::getServiceContext() {
    return getGlobalServiceContext();
}


ServiceContextTest::ServiceContextTest() : _threadClient(getServiceContext()) {}

ServiceContextTest::~ServiceContextTest() = default;

Client* ServiceContextTest::getClient() {
    return Client::getCurrent();
}

}  // namespace mongo
