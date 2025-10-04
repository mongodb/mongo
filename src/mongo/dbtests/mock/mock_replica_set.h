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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace MONGO_MOD_PUB mongo {

class ClockSource;

/**
 * This is a helper class for managing a replica set consisting of
 * MockRemoteDBServer instances.
 *
 * Note: Be sure to call ScopedDbConnection::clearPool() after every test
 * when doing tests that involves the ReplicaSetMonitor. This is because
 * it uses ScopedDbConnection which means you can have a residue connections
 * that was created from previous tests and can cause a seg fault if the
 * MockRemoteDBServer instances were already destroyed.
 *
 * Warning: Not thread-safe
 */
class MockReplicaSet {
public:
    /**
     * Creates a mock replica set and automatically mocks the hello and replSetGetStatus commands
     * based on the default replica set configuration. Either the first node is primary and the
     * others are secondaries, or all are secondaries. By default, hostnames begin with "$", which
     * signals to ReplicaSetMonitor and to ConnectionString::connect that these are mocked hosts.
     *
     * @param setName The name for this replica set
     * @param nodes The initial number of nodes for this replica set
     * @param hasPrimary Whether the first node is primary or all are secondaries
     * @param dollarPrefixHosts Whether hostnames should begin with "$"
     */
    MockReplicaSet(const std::string& setName,
                   size_t nodes,
                   bool hasPrimary = true,
                   bool dollarPrefixHosts = true);
    ~MockReplicaSet();

    //
    // getters
    //

    std::string getSetName() const;
    std::string getConnectionString() const;
    MongoURI getURI() const;
    std::vector<HostAndPort> getHosts() const;
    repl::ReplSetConfig getReplConfig() const;
    bool hasPrimary() const;
    std::string getPrimary() const;
    std::vector<std::string> getSecondaries() const;

    /**
     * Sets the configuration for this replica sets. This also has a side effect of mocking the
     * hello and replSetGetStatus command responses based on the new config.
     *
     * Note: does not automatically select a new primary. Can be done manually by calling
     * setPrimary.
     */
    void setConfig(const repl::ReplSetConfig& newConfig);

    /**
     * Mark one of the config members as primary. Pass the empty string if all nodes are secondary.
     */
    void setPrimary(const std::string& hostAndPort);

    /**
     * @return pointer to the mocked remote server with the given hostName.
     *     NULL if host doesn't exists.
     */
    MockRemoteDBServer* getNode(const std::string& hostAndPort);
    const MockRemoteDBServer* getNode(const std::string& hostAndPort) const;

    /**
     * Kills a node belonging to this set.
     *
     * @param hostName the name of the replica node to kill.
     */
    void kill(const std::string& hostAndPort);

    /**
     * Kills a set of host belonging to this set.
     *
     * @param hostList the list of host names of the servers to kill.
     */
    void kill(const std::vector<std::string>& hostList);

    /**
     * Reboots a node.
     *
     * @param hostName the name of the host to reboot.
     */
    void restore(const std::string& hostName);

    /**
     * Returns a topology description reflecting the current state of this replica set.
     */
    sdam::TopologyDescriptionPtr getTopologyDescription(ClockSource* clockSource) const;

private:
    typedef std::map<std::string, MockRemoteDBServer*> ReplNodeMap;

    /**
     * Mocks the "hello" command based on the information on the current replica set configuration.
     */
    void mockHelloCmd();

    /**
     * Mock the hello response for the given server.
     */
    BSONObj mockHelloResponseFor(const MockRemoteDBServer& server) const;

    /**
     * Mocks the replSetGetStatus command based on the current states of the
     * mocked servers.
     */
    void mockReplSetGetStatusCmd();

    /**
     * @return the replica set state of the given host
     */
    int getState(const std::string& hostAndPort) const;

    const std::string _setName;
    ReplNodeMap _nodeMap;
    repl::ReplSetConfig _replConfig;

    std::string _primaryHost;
};
}  // namespace MONGO_MOD_PUB mongo
