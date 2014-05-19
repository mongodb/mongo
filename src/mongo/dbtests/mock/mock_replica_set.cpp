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

#include "mongo/dbtests/mock/mock_replica_set.h"

#include "mongo/db/repl/member_state.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/util/map_util.h"

#include <sstream>

namespace mongo {
    MockReplicaSet::MockReplicaSet(const string& setName, size_t nodes):
            _setName(setName) {
        ReplConfigMap replConfig;

        for (size_t n = 0; n < nodes; n++) {
            std::stringstream str;
            str << "$" << setName << n << ":27017";
            const string hostName(str.str());

            if (n == 0) {
                _primaryHost = hostName;
            }

            MockRemoteDBServer* mockServer = new MockRemoteDBServer(hostName);
            _nodeMap[hostName] = mockServer;

            MockConnRegistry::get()->addServer(mockServer);

            replset::ReplSetConfig::MemberCfg config;
            config.h = HostAndPort(hostName);
            replConfig.insert(std::make_pair(hostName, config));
        }

        setConfig(replConfig);
    }

    MockReplicaSet::~MockReplicaSet() {
        for (ReplNodeMap::iterator iter = _nodeMap.begin();
                iter != _nodeMap.end(); ++iter) {
            MockConnRegistry::get()->removeServer(iter->second->getServerAddress());
            delete iter->second;
        }
    }

    string MockReplicaSet::getSetName() const {
        return _setName;
    }

    string MockReplicaSet::getConnectionString() const {
        std::stringstream str;
        str << _setName;
        str << "/";

        ReplNodeMap::const_iterator iter = _nodeMap.begin();
        while (iter != _nodeMap.end()) {
            str << iter->second->getServerAddress();
            ++iter;

            if (iter != _nodeMap.end()) {
                str << ",";
            }
        }

        return str.str();
    }

    vector<HostAndPort> MockReplicaSet::getHosts() const {
        vector<HostAndPort> list;

        for (ReplNodeMap::const_iterator iter = _nodeMap.begin();
                iter != _nodeMap.end(); ++iter) {
            list.push_back(HostAndPort(iter->second->getServerAddress()));
        }

        return list;
    }

    string MockReplicaSet::getPrimary() const {
        return _primaryHost;
    }

    void MockReplicaSet::setPrimary(const string& hostAndPort) {
        ReplConfigMap::const_iterator iter = _replConfig.find(hostAndPort);
        fassert(16578, iter != _replConfig.end());

        const replset::ReplSetConfig::MemberCfg& config = iter->second;
        fassert(16579, !config.hidden && config.priority > 0 && !config.arbiterOnly);

        _primaryHost = hostAndPort;

        mockIsMasterCmd();
        mockReplSetGetStatusCmd();
    }

    vector<string> MockReplicaSet::getSecondaries() const {
        vector<string> secondaries;

        for (ReplConfigMap::const_iterator iter = _replConfig.begin();
                iter != _replConfig.end(); ++iter) {
            if (iter->first != _primaryHost) {
                secondaries.push_back(iter->first);
            }
        }

        return secondaries;
    }

    MockRemoteDBServer* MockReplicaSet::getNode(const string& hostAndPort) {
        return mapFindWithDefault(_nodeMap, hostAndPort, static_cast<MockRemoteDBServer*>(NULL));
    }

    MockReplicaSet::ReplConfigMap MockReplicaSet::getReplConfig() const {
        return _replConfig;
    }

    void MockReplicaSet::setConfig(const MockReplicaSet::ReplConfigMap& newConfig) {
        _replConfig = newConfig;
        mockIsMasterCmd();
        mockReplSetGetStatusCmd();
    }

    void MockReplicaSet::kill(const string& hostAndPort) {
        verify(_nodeMap.count(hostAndPort) == 1);
        _nodeMap[hostAndPort]->shutdown();
    }

    void MockReplicaSet::kill(const vector<string>& hostList) {
        for (vector<string>::const_iterator iter = hostList.begin();
                iter != hostList.end(); ++iter) {
            kill(*iter);
        }
    }

    void MockReplicaSet::restore(const string& hostAndPort) {
        verify(_nodeMap.count(hostAndPort) == 1);
        _nodeMap[hostAndPort]->reboot();
    }

    void MockReplicaSet::mockIsMasterCmd() {
        // Copied from ReplSetImpl::_fillIsMaster
        for (ReplNodeMap::iterator nodeIter = _nodeMap.begin();
                nodeIter != _nodeMap.end(); ++nodeIter) {
            const string& hostAndPort = nodeIter->first;

            BSONObjBuilder builder;
            builder.append("setName", _setName);

            ReplConfigMap::const_iterator configIter = _replConfig.find(hostAndPort);
            if (configIter == _replConfig.end()) {
                builder.append("ismaster", false);
                builder.append("secondary", false);

                vector<string> hostList;
                builder.append("hosts", hostList);
            }
            else {
                const bool isPrimary = hostAndPort == getPrimary();
                builder.append("ismaster", isPrimary);
                builder.append("secondary", !isPrimary);

                {
                    // TODO: add passives & arbiters
                    vector<string> hostList;
                    hostList.push_back(getPrimary());

                    const vector<string> secondaries = getSecondaries();
                    for (vector<string>::const_iterator secIter = secondaries.begin();
                                    secIter != secondaries.end(); ++secIter) {
                        hostList.push_back(*secIter);
                    }

                    builder.append("hosts", hostList);
                }

                builder.append("primary", getPrimary());

                const replset::ReplSetConfig::MemberCfg& replConfig = configIter->second;
                if (replConfig.arbiterOnly) {
                    builder.append("arbiterOnly", true);
                }

                if (replConfig.priority == 0 && !replConfig.arbiterOnly) {
                    builder.append("passive", true);
                }

                if (replConfig.slaveDelay) {
                    builder.append("slaveDelay", replConfig.slaveDelay);
                }

                if (replConfig.hidden) {
                    builder.append("hidden", true);
                }

                if (!replConfig.buildIndexes) {
                    builder.append("buildIndexes", false);
                }

                if(!replConfig.tags.empty()) {
                    BSONObjBuilder tagBuilder;
                    for(map<string, string>::const_iterator tagIter = replConfig.tags.begin();
                            tagIter != replConfig.tags.end(); tagIter++) {
                        tagBuilder.append(tagIter->first, tagIter->second);
                    }

                    builder.append("tags", tagBuilder.done());
                }
            }

            builder.append("me", hostAndPort);
            builder.append("ok", true);

            nodeIter->second->setCommandReply("ismaster", builder.done());
        }
    }

    int MockReplicaSet::getState(const std::string& hostAndPort) const {
        if (_replConfig.count(hostAndPort) < 1) {
            return static_cast<int>(replset::MemberState::RS_SHUNNED);
        }
        else if (hostAndPort == getPrimary()) {
            return static_cast<int>(replset::MemberState::RS_PRIMARY);
        }
        else {
            return static_cast<int>(replset::MemberState::RS_SECONDARY);
        }
    }

    void MockReplicaSet::mockReplSetGetStatusCmd() {
        // Copied from ReplSetImpl::_summarizeStatus
        for (ReplNodeMap::iterator nodeIter = _nodeMap.begin();
                nodeIter != _nodeMap.end(); ++nodeIter) {
            MockRemoteDBServer* node = nodeIter->second;
            vector<BSONObj> hostsField;

            BSONObjBuilder fullStatBuilder;

            {
                BSONObjBuilder selfStatBuilder;
                selfStatBuilder.append("name", node->getServerAddress());
                selfStatBuilder.append("health", 1.0);
                selfStatBuilder.append("state", getState(node->getServerAddress()));

                selfStatBuilder.append("self", true);
                // TODO: _id, stateStr, uptime, optime, optimeDate, maintenanceMode, errmsg

                hostsField.push_back(selfStatBuilder.obj());
            }

            for (ReplConfigMap::const_iterator replConfIter = _replConfig.begin();
                    replConfIter != _replConfig.end(); ++replConfIter) {
                MockRemoteDBServer* hostNode = getNode(replConfIter->first);

                if (hostNode == node) {
                    continue;
                }

                BSONObjBuilder hostMemberBuilder;

                // TODO: _id, stateStr, uptime, optime, optimeDate, lastHeartbeat, pingMs
                // errmsg, authenticated

                hostMemberBuilder.append("name", hostNode->getServerAddress());
                const double health = hostNode->isRunning() ? 1.0 : 0.0;
                hostMemberBuilder.append("health", health);
                hostMemberBuilder.append("state", getState(hostNode->getServerAddress()));

                hostsField.push_back(hostMemberBuilder.obj());
            }

            sort(hostsField.begin(), hostsField.end());

            // TODO: syncingTo

            fullStatBuilder.append("set", _setName);
            fullStatBuilder.appendTimeT("date", time(0));
            fullStatBuilder.append("myState", getState(node->getServerAddress()));
            fullStatBuilder.append("members", hostsField);
            fullStatBuilder.append("ok", true);

            node->setCommandReply("replSetGetStatus", fullStatBuilder.done());
        }
    }
}
