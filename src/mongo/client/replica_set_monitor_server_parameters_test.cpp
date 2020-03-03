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

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/scanning_replica_set_monitor.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * Tests the replicaSetMonitorProtocol server parameter.
 */
class ReplicaSetMonitorProtocolTest : public unittest::Test {
protected:
    void setUp() {
        setGlobalServiceContext(ServiceContext::make());
        ReplicaSetMonitor::cleanup();
    }

    void tearDown() {
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

/**
 * Checks that a ScanningReplicaSetMonitor is created when the replicaSetMonitorProtocol server
 * parameter is set to 'scanning'.
 */
TEST_F(ReplicaSetMonitorProtocolTest, checkRSMProtocolParamScanning) {
    ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kScanning);
    auto uri = MongoURI::parse("mongodb:a,b,c/?replicaSet=name");
    ASSERT_OK(uri.getStatus());
    auto createdMonitor = ReplicaSetMonitor::createIfNeeded(uri.getValue());

    // If the created monitor does not point to a ScanningReplicaSetMonitor, the cast returns a
    // nullptr.
    auto scanningMonitorCast = dynamic_cast<ScanningReplicaSetMonitor*>(createdMonitor.get());
    ASSERT(scanningMonitorCast);

    auto streamableMonitorCast = dynamic_cast<StreamableReplicaSetMonitor*>(createdMonitor.get());
    ASSERT_FALSE(streamableMonitorCast);
}
}  // namespace
}  // namespace mongo
