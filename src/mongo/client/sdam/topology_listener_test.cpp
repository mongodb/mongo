/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/client/sdam/topology_listener.h"

#include "mongo/base/string_data.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/sdam/topology_listener_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <ostream>
#include <utility>

namespace mongo {
namespace {

class TopologyListenerTestFixture : public unittest::Test {
protected:
    /**
     * Sets up the task executor as well as a TopologyListenerMock for each unit test.
     */
    void setUp() override {
        auto serviceContext = ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContext));
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kSdam);
        ReplicaSetMonitor::cleanup();

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeThreadPoolTestExecutor(std::move(network));
        _executor->startup();
        _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
    }

    void tearDown() override {
        _eventsPublisher.reset();
        _executor->shutdown();
        _executor->join();
        _executor.reset();
        ReplicaSetMonitor::cleanup();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
    }

    std::shared_ptr<sdam::TopologyEventsPublisher> getEventsPublisher() {
        return _eventsPublisher;
    }

private:
    std::shared_ptr<sdam::TopologyEventsPublisher> _eventsPublisher;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _net;
};

TEST_F(TopologyListenerTestFixture, TestListeners) {
    std::shared_ptr<sdam::TopologyListenerMock> l1 = std::make_shared<sdam::TopologyListenerMock>();
    std::shared_ptr<sdam::TopologyListenerMock> l2 = std::make_shared<sdam::TopologyListenerMock>();

    getEventsPublisher()->registerListener(l1);
    getEventsPublisher()->registerListener(l2);

    getEventsPublisher()->onServerPingSucceededEvent(Microseconds(1), HostAndPort("abc.def:3421"));

    while (!l1->hasPingResponse(HostAndPort("abc.def:3421")) ||
           !l2->hasPingResponse(HostAndPort("abc.def:3421"))) {
        sleepFor(Milliseconds(1));
    }
    ASSERT_TRUE(l1->hasPingResponse(HostAndPort("abc.def:3421")));
    ASSERT_TRUE(l2->hasPingResponse(HostAndPort("abc.def:3421")));
}

// Tests that event publisher handles listener weak pointers properly.
TEST_F(TopologyListenerTestFixture, TestListenersRefCounts) {
    std::shared_ptr<sdam::TopologyListenerMock> l1 = std::make_shared<sdam::TopologyListenerMock>();
    std::shared_ptr<sdam::TopologyListenerMock> l2 = std::make_shared<sdam::TopologyListenerMock>();

    getEventsPublisher()->registerListener(l1);
    getEventsPublisher()->registerListener(l2);

    ASSERT_EQ(1, l1.use_count()) << "Listener should have only one owner now";

    getEventsPublisher()->onServerPingSucceededEvent(Microseconds(1), HostAndPort("abc.def:1"));

    // Wait for asynchronous event processing to complete and release the shared_ptr.
    do {
        // Even this has a tiny chance of race because the event is delivered before the
        // shared_ptr is released.
        sleepFor(Milliseconds(1));
    } while (!l1->hasPingResponse(HostAndPort("abc.def:1")) && 1 != l1.use_count());

    // Deletes the l1 listener, as it has only one owner. The next event should not
    // trigger any ASAN/TSAN error as the weak pointer is handled properly.
    l1.reset();

    getEventsPublisher()->onServerPingSucceededEvent(Microseconds(1), HostAndPort("abc.def:2"));

    while (!l2->hasPingResponse(HostAndPort("abc.def:2"))) {
        sleepFor(Milliseconds(1));
    }
    ASSERT_TRUE(l2->hasPingResponse(HostAndPort("abc.def:2")));
}

}  // namespace
}  // namespace mongo
