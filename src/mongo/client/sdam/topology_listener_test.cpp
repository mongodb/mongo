// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sdam/topology_listener.h"

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
