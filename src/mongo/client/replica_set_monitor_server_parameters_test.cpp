// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/replica_set_monitor_server_parameters.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Tests the replicaSetMonitorProtocol server parameter.
 */
class ReplicaSetMonitorProtocolTest : public unittest::Test {
protected:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        ReplicaSetMonitor::cleanup();
    }

    void tearDown() override {
        ReplicaSetMonitor::cleanup();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
    }
};

#if 0
/**
 * Checks that a StreamableReplicaSetMonitor is created when the replicaSetMonitorProtocol server
 * parameter is set to 'sdam'.
 */
TEST_F(ReplicaSetMonitorProtocolTest, checkRSMProtocolParamSdam) {
    ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kSdam);
    auto uri = MongoURI::parse("mongodb:a,b,c/?replicaSet=name");
    ASSERT_OK(uri.getStatus());
    auto createdMonitor = ReplicaSetMonitor::createIfNeeded(uri.getValue());

    // If the created monitor does not point to a ScanningReplicaSetMonitor, the cast returns a
    // nullptr.
    auto streamableMonitorCast = dynamic_cast<StreamableReplicaSetMonitor*>(createdMonitor.get());
    ASSERT(streamableMonitorCast);

    auto scanningMonitorCast = dynamic_cast<ScanningReplicaSetMonitor*>(createdMonitor.get());
    ASSERT_FALSE(scanningMonitorCast);
}
#endif

}  // namespace
}  // namespace mongo
