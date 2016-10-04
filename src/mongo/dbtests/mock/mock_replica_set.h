/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"

#include <map>
#include <string>
#include <vector>

namespace mongo {
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
     * Creates a mock replica set and automatically mocks the isMaster
     * and replSetGetStatus commands based on the default replica set
     * configuration.
     *
     * @param setName The name for this replica set
     * @param nodes The initial number of nodes for this replica set
     */
    MockReplicaSet(const std::string& setName, size_t nodes);
    ~MockReplicaSet();

    //
    // getters
    //

    std::string getSetName() const;
    std::string getConnectionString() const;
    std::vector<HostAndPort> getHosts() const;
    repl::ReplicaSetConfig getReplConfig() const;
    std::string getPrimary() const;
    std::vector<std::string> getSecondaries() const;

    /**
     * Sets the configuration for this replica sets. This also has a side effect
     * of mocking the ismaster and replSetGetStatus command responses based on
     * the new config.
     *
     * Note: does not automatically select a new primary. Can be done manually by
     * calling setPrimary.
     */
    void setConfig(const repl::ReplicaSetConfig& newConfig);

    void setPrimary(const std::string& hostAndPort);

    /**
     * @return pointer to the mocked remote server with the given hostName.
     *     NULL if host doesn't exists.
     */
    MockRemoteDBServer* getNode(const std::string& hostAndPort);

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

private:
    typedef std::map<std::string, MockRemoteDBServer*> ReplNodeMap;

    /**
     * Mocks the ismaster command based on the information on the current
     * replica set configuration.
     */
    void mockIsMasterCmd();

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
    repl::ReplicaSetConfig _replConfig;

    std::string _primaryHost;
};
}
