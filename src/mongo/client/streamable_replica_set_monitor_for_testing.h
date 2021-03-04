/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/client/sdam/mock_topology_manager.h"
#include "mongo/client/streamable_replica_set_monitor.h"

namespace mongo {

/**
 * Sets up a streamable replica set monitor for testing that does not automatically refresh on its
 * own, and with configurable topology.
 */
class StreamableReplicaSetMonitorForTesting {
public:
    ~StreamableReplicaSetMonitorForTesting();

    void setup(const MongoURI& uri);

    sdam::MockTopologyManager* getTopologyManager();

private:
    // Executor for monitoring replica sets.
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    // Allows closing connections established by the network interface associated with the
    // _taskExecutor instance
    std::shared_ptr<ReplicaSetMonitorConnectionManager> _connectionManager;

    // Allows changing replica set topology for mocking replica set targetter response.
    sdam::MockTopologyManager* _topologyManagerPtr;

    // ReplicaSetMonitorManager only stores weak_ptrs, so we need to keep the monitor alive.
    std::shared_ptr<StreamableReplicaSetMonitor> _replSetMonitor;
};

}  // namespace mongo
