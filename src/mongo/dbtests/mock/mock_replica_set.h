// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
}  // namespace mongo
