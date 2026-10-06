// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/system_clock_source.h"

#include <memory>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using unittest::match::StatusIs;

const HostAndPort kHost{"localhost", 27017};
const BSONObj kSecondaryHelloReply = BSON("ok" << 1 << "secondary" << true << "setName"
                                               << "testSet");

class NoOpConnectionManager : public executor::EgressConnectionCloser {
public:
    void dropConnections(const Status&) override {}
    void dropConnections(const HostAndPort&, const Status&) override {}
    void setKeepOpen(const HostAndPort&, bool) override {}
};

class RecordingConnectionManager : public executor::EgressConnectionCloser {
public:
    void dropConnections(const Status&) override {
        ++dropAllCount;
    }
    void dropConnections(const HostAndPort& host, const Status& status) override {
        droppedHosts.push_back(host);
        lastStatus = status;
    }
    void setKeepOpen(const HostAndPort&, bool) override {}

    std::vector<HostAndPort> droppedHosts;
    int dropAllCount = 0;
    Status lastStatus = Status::OK();
};

class OnServerHeartbeatFailureEventTest : service_context_test::WithSetupTransportLayer,
                                          public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = executor::makeThreadPoolTestExecutor(std::move(network));
        _executor->startup();

        const auto uri =
            uassertStatusOK(MongoURI::parse("mongodb://localhost:27017/?replicaSet=testSet"));
        _connectionManager = std::make_shared<NoOpConnectionManager>();
        auto stats = std::make_shared<ReplicaSetMonitorManagerStats>();
        _monitor = std::make_shared<StreamableReplicaSetMonitor>(
            uri, _executor, _connectionManager, [] {}, stats);

        sdam::SdamConfiguration sdamConfig(std::vector<HostAndPort>{kHost},
                                           sdam::TopologyType::kReplicaSetNoPrimary,
                                           std::string("testSet"));
        auto topologyMgr =
            std::make_unique<sdam::TopologyManagerImpl>(sdamConfig, SystemClockSource::get());
        _topologyManagerImpl = topologyMgr.get();
        _monitor->initForTesting(std::move(topologyMgr));

        // Start by setting the topology description to secondary for tests to compare against.
        setServerToSecondary();
    }

    void tearDown() override {
        _monitor->drop();
        _monitor.reset();
        _executor->shutdown();
        executor::NetworkInterfaceMock::InNetworkGuard(_net)->runReadyNetworkOperations();
        _executor->join();
        ServiceContextTest::tearDown();
    }

protected:
    // onServerHeartbeatFailureEvent is private on StreamableReplicaSetMonitor but public on the
    // TopologyListener base, so we call it through the interface.
    void triggerHeartbeatFailure(const Status& status,
                                 const HostAndPort& host,
                                 const BSONObj& reply) {
        static_cast<sdam::TopologyListener*>(_monitor.get())
            ->onServerHeartbeatFailureEvent(status, host, reply);
    }

    void setServerToSecondary() {
        sdam::HelloOutcome success(kHost, kSecondaryHelloReply);
        _topologyManagerImpl->onServerDescription(success);
        ASSERT_EQUALS(sdam::ServerType::kRSSecondary, getServerType(kHost));
    }

    sdam::ServerType getServerType(const HostAndPort& host) {
        auto serverDesc = _topologyManagerImpl->getTopologyDescription()->findServerByAddress(host);
        ASSERT(serverDesc);
        return (*serverDesc)->getType();
    }

    sdam::TopologyManagerImpl* _topologyManagerImpl{nullptr};
    std::shared_ptr<StreamableReplicaSetMonitor> _monitor;
    std::shared_ptr<NoOpConnectionManager> _connectionManager;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _net{nullptr};
};

// A non-retriable error marks the server as Unknown.
TEST_F(OnServerHeartbeatFailureEventTest, NonRetriableErrorMarksServerUnknown) {
    triggerHeartbeatFailure(Status(ErrorCodes::UnknownError, "error"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kUnknown, getServerType(kHost));
}

// A local NetworkError will only update the ServerType after failing twice in a row.
TEST_F(OnServerHeartbeatFailureEventTest, NetworkErrorMarksServerUnknown) {
    triggerHeartbeatFailure(Status(ErrorCodes::SocketException, "socket error"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kRSSecondary, getServerType(kHost));

    triggerHeartbeatFailure(Status(ErrorCodes::SocketException, "socket error"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kUnknown, getServerType(kHost));
}

// Any remote error marks the server as Unknown.
TEST_F(OnServerHeartbeatFailureEventTest, ShutdownErrorMarksServerUnknown) {
    const auto shutdownReply =
        BSON("ok" << 0 << "code" << static_cast<int>(ErrorCodes::ShutdownInProgress));
    triggerHeartbeatFailure(
        Status(ErrorCodes::ShutdownInProgress, "Shutting down"), kHost, shutdownReply);
    ASSERT_EQUALS(sdam::ServerType::kUnknown, getServerType(kHost));

    // Reset server type.
    setServerToSecondary();

    const auto retriableReply =
        BSON("ok" << 0 << "code" << static_cast<int>(ErrorCodes::RetriableRemoteCommandFailure));
    triggerHeartbeatFailure(
        Status(ErrorCodes::RetriableRemoteCommandFailure, "err"), kHost, retriableReply);
    ASSERT_EQUALS(sdam::ServerType::kUnknown, getServerType(kHost));
}

// Any local retriable error that isn't NetworkError will not update server description.
TEST_F(OnServerHeartbeatFailureEventTest, DoesNotMarkServerUnknown) {
    triggerHeartbeatFailure(
        Status(ErrorCodes::RetriableRemoteCommandFailure, "error1"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kRSSecondary, getServerType(kHost));

    triggerHeartbeatFailure(Status(ErrorCodes::NotWritablePrimary, "error2"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kRSSecondary, getServerType(kHost));

    triggerHeartbeatFailure(Status(ErrorCodes::ExceededTimeLimit, "error3"), kHost, BSONObj());
    ASSERT_EQUALS(sdam::ServerType::kRSSecondary, getServerType(kHost));
}

using OnTopologyChangeEventTest = OnServerHeartbeatFailureEventTest;

// Hiding a member removes it from the topology.
TEST_F(OnTopologyChangeEventTest, DropsConnectionsWhenHostLeavesTopology) {
    const HostAndPort host0{"localhost", 27017};
    const HostAndPort host1{"localhost", 27018};
    auto connectionManager = std::make_shared<RecordingConnectionManager>();
    const auto uri = uassertStatusOK(
        MongoURI::parse("mongodb://localhost:27017,localhost:27018/?replicaSet=testSet"));
    auto monitor = std::make_shared<StreamableReplicaSetMonitor>(
        uri,
        _executor,
        connectionManager,
        [] {},
        std::make_shared<ReplicaSetMonitorManagerStats>());

    sdam::SdamConfiguration sdamConfig(std::vector<HostAndPort>{host0},
                                       sdam::TopologyType::kReplicaSetNoPrimary,
                                       std::string("testSet"));
    monitor->initForTesting(
        std::make_unique<sdam::TopologyManagerImpl>(sdamConfig, SystemClockSource::get()));

    sdam::SdamConfiguration bothHosts(std::vector<HostAndPort>{host0, host1},
                                      sdam::TopologyType::kReplicaSetNoPrimary,
                                      std::string("testSet"));
    auto previous = sdam::TopologyDescription::create(bothHosts);
    auto unchanged = sdam::TopologyDescription::clone(*previous);
    static_cast<sdam::TopologyListener*>(monitor.get())
        ->onTopologyDescriptionChangedEvent(previous, unchanged);
    EXPECT_THAT(connectionManager->droppedHosts, IsEmpty());

    auto withoutHost1 = sdam::TopologyDescription::clone(*previous);
    withoutHost1->removeServerDescription(host1);
    static_cast<sdam::TopologyListener*>(monitor.get())
        ->onTopologyDescriptionChangedEvent(previous, withoutHost1);

    EXPECT_THAT(connectionManager->droppedHosts, ElementsAre(host1));
    EXPECT_THAT(connectionManager->lastStatus,
                StatusIs(Eq(ErrorCodes::PooledConnectionsDropped),
                         Eq("Host was removed from the replica set topology")));
    EXPECT_THAT(connectionManager->dropAllCount, Eq(0));

    monitor->drop();
}

}  // namespace
}  // namespace mongo
