/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/db/repl/rs_config.h"

#include <string>
#include <map>
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
        typedef std::map<std::string, ReplSetConfig::MemberCfg> ReplConfigMap;

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
        ReplConfigMap getReplConfig() const;
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
        void setConfig(const ReplConfigMap& newConfig);

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
        ReplConfigMap _replConfig;

        std::string _primaryHost;
    };
}
