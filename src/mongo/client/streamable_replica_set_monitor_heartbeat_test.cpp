/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

const HostAndPort kHost{"localhost", 27017};
const BSONObj kSecondaryHelloReply = BSON("ok" << 1 << "secondary" << true << "setName"
                                               << "testSet");

class NoOpConnectionManager : public executor::EgressConnectionCloser {
public:
    void dropConnections(const Status&) override {}
    void dropConnections(const HostAndPort&, const Status&) override {}
    void setKeepOpen(const HostAndPort&, bool) override {}
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

}  // namespace
}  // namespace mongo
