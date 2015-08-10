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

using namespace mongo::repl;

namespace mongo {

using std::string;
using std::vector;

MockReplicaSet::MockReplicaSet(const string& setName, size_t nodes) : _setName(setName) {
    BSONObjBuilder configBuilder;
    configBuilder.append("_id", setName);
    configBuilder.append("version", 1);

    BSONArrayBuilder membersBuilder(configBuilder.subarrayStart("members"));
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

        membersBuilder.append(BSON("_id" << static_cast<int>(n) << "host" << hostName));
    }
    membersBuilder.done();

    ReplicaSetConfig replConfig;
    fassert(28566, replConfig.initialize(configBuilder.obj()));
    fassert(28573, replConfig.validate());
    setConfig(replConfig);
}

MockReplicaSet::~MockReplicaSet() {
    for (ReplNodeMap::iterator iter = _nodeMap.begin(); iter != _nodeMap.end(); ++iter) {
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

    for (ReplNodeMap::const_iterator iter = _nodeMap.begin(); iter != _nodeMap.end(); ++iter) {
        list.push_back(HostAndPort(iter->second->getServerAddress()));
    }

    return list;
}

string MockReplicaSet::getPrimary() const {
    return _primaryHost;
}

void MockReplicaSet::setPrimary(const string& hostAndPort) {
    const MemberConfig* config = _replConfig.findMemberByHostAndPort(HostAndPort(hostAndPort));
    fassert(16578, config);

    fassert(16579, !config->isHidden() && config->getPriority() > 0 && !config->isArbiter());

    _primaryHost = hostAndPort;

    mockIsMasterCmd();
    mockReplSetGetStatusCmd();
}

vector<string> MockReplicaSet::getSecondaries() const {
    vector<string> secondaries;

    for (ReplicaSetConfig::MemberIterator member = _replConfig.membersBegin();
         member != _replConfig.membersEnd();
         ++member) {
        if (member->getHostAndPort() != HostAndPort(_primaryHost)) {
            secondaries.push_back(member->getHostAndPort().toString());
        }
    }

    return secondaries;
}

MockRemoteDBServer* MockReplicaSet::getNode(const string& hostAndPort) {
    return mapFindWithDefault(_nodeMap, hostAndPort, static_cast<MockRemoteDBServer*>(NULL));
}

repl::ReplicaSetConfig MockReplicaSet::getReplConfig() const {
    return _replConfig;
}

void MockReplicaSet::setConfig(const repl::ReplicaSetConfig& newConfig) {
    _replConfig = newConfig;
    mockIsMasterCmd();
    mockReplSetGetStatusCmd();
}

void MockReplicaSet::kill(const string& hostAndPort) {
    verify(_nodeMap.count(hostAndPort) == 1);
    _nodeMap[hostAndPort]->shutdown();
}

void MockReplicaSet::kill(const vector<string>& hostList) {
    for (vector<string>::const_iterator iter = hostList.begin(); iter != hostList.end(); ++iter) {
        kill(*iter);
    }
}

void MockReplicaSet::restore(const string& hostAndPort) {
    verify(_nodeMap.count(hostAndPort) == 1);
    _nodeMap[hostAndPort]->reboot();
}

void MockReplicaSet::mockIsMasterCmd() {
    for (ReplNodeMap::iterator nodeIter = _nodeMap.begin(); nodeIter != _nodeMap.end();
         ++nodeIter) {
        const string& hostAndPort = nodeIter->first;

        BSONObjBuilder builder;
        builder.append("setName", _setName);

        const MemberConfig* member = _replConfig.findMemberByHostAndPort(HostAndPort(hostAndPort));
        if (!member) {
            builder.append("ismaster", false);
            builder.append("secondary", false);

            vector<string> hostList;
            builder.append("hosts", hostList);
        } else {
            const bool isPrimary = hostAndPort == getPrimary();
            builder.append("ismaster", isPrimary);
            builder.append("secondary", !isPrimary);

            {
                // TODO: add passives & arbiters
                vector<string> hostList;
                hostList.push_back(getPrimary());

                const vector<string> secondaries = getSecondaries();
                for (vector<string>::const_iterator secIter = secondaries.begin();
                     secIter != secondaries.end();
                     ++secIter) {
                    hostList.push_back(*secIter);
                }

                builder.append("hosts", hostList);
            }

            builder.append("primary", getPrimary());

            if (member->isArbiter()) {
                builder.append("arbiterOnly", true);
            }

            if (member->getPriority() == 0 && !member->isArbiter()) {
                builder.append("passive", true);
            }

            if (member->getSlaveDelay().count()) {
                builder.appendIntOrLL("slaveDelay",
                                      durationCount<Seconds>(member->getSlaveDelay()));
            }

            if (member->isHidden()) {
                builder.append("hidden", true);
            }

            if (!member->shouldBuildIndexes()) {
                builder.append("buildIndexes", false);
            }

            const ReplicaSetTagConfig tagConfig = _replConfig.getTagConfig();
            if (member->hasTags(tagConfig)) {
                BSONObjBuilder tagBuilder;
                for (MemberConfig::TagIterator tag = member->tagsBegin(); tag != member->tagsEnd();
                     ++tag) {
                    std::string tagKey = tagConfig.getTagKey(*tag);
                    if (tagKey[0] == '$') {
                        // Filter out internal tags
                        continue;
                    }
                    tagBuilder.append(tagKey, tagConfig.getTagValue(*tag));
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
    if (!_replConfig.findMemberByHostAndPort(HostAndPort(hostAndPort))) {
        return static_cast<int>(MemberState::RS_REMOVED);
    } else if (hostAndPort == getPrimary()) {
        return static_cast<int>(MemberState::RS_PRIMARY);
    } else {
        return static_cast<int>(MemberState::RS_SECONDARY);
    }
}

void MockReplicaSet::mockReplSetGetStatusCmd() {
    // Copied from ReplSetImpl::_summarizeStatus
    for (ReplNodeMap::iterator nodeIter = _nodeMap.begin(); nodeIter != _nodeMap.end();
         ++nodeIter) {
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

        for (ReplicaSetConfig::MemberIterator member = _replConfig.membersBegin();
             member != _replConfig.membersEnd();
             ++member) {
            MockRemoteDBServer* hostNode = getNode(member->getHostAndPort().toString());

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
