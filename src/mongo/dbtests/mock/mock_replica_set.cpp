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

#include "mongo/dbtests/mock/mock_replica_set.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description_builder.h"
#include "mongo/db/repl/member_config.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_set_tag.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <sstream>
#include <utility>

#include <boost/move/utility_core.hpp>

using namespace mongo::repl;

namespace mongo {

using std::string;
using std::vector;

MockReplicaSet::MockReplicaSet(const string& setName,
                               size_t nodes,
                               bool hasPrimary,
                               bool dollarPrefixHosts)
    : _setName(setName) {
    invariant(nodes > 0);
    BSONObjBuilder configBuilder;
    configBuilder.append("_id", setName);
    configBuilder.append("version", 1);
    configBuilder.append("protocolVersion", 1);

    BSONArrayBuilder membersBuilder(configBuilder.subarrayStart("members"));
    // If e.g. setName="rs" and dollarPrefixHosts=true, make hostnames like "$rs0:27017,$rs1:27017".
    for (size_t n = 0; n < nodes; n++) {
        std::stringstream str;
        if (dollarPrefixHosts) {
            str << "$";
        }
        str << setName << n << ":27017";
        const string hostName(str.str());

        if (n == 0 && hasPrimary) {
            _primaryHost = hostName;
        }

        MockRemoteDBServer* mockServer = new MockRemoteDBServer(hostName);
        _nodeMap[hostName] = mockServer;

        MockConnRegistry::get()->addServer(mockServer);

        membersBuilder.append(BSON("_id" << static_cast<int>(n) << "host" << hostName));
    }
    membersBuilder.done();

    ReplSetConfig replConfig;
    try {
        replConfig = ReplSetConfig::parse(configBuilder.obj());
    } catch (const DBException&) {
        fassertFailed(28566);
    }
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

MongoURI MockReplicaSet::getURI() const {
    return invariant(MongoURI::parse(getConnectionString()));
}

vector<HostAndPort> MockReplicaSet::getHosts() const {
    vector<HostAndPort> list;

    for (ReplNodeMap::const_iterator iter = _nodeMap.begin(); iter != _nodeMap.end(); ++iter) {
        list.push_back(HostAndPort(iter->second->getServerAddress()));
    }

    return list;
}

bool MockReplicaSet::hasPrimary() const {
    return !_primaryHost.empty();
}

string MockReplicaSet::getPrimary() const {
    return _primaryHost;
}

void MockReplicaSet::setPrimary(const string& hostAndPort) {
    if (!hostAndPort.empty()) {
        const MemberConfig* config = _replConfig.findMemberByHostAndPort(HostAndPort(hostAndPort));
        fassert(16578, config);

        fassert(16579, !config->isHidden() && config->getPriority() > 0 && !config->isArbiter());
    }

    _primaryHost = hostAndPort;

    mockHelloCmd();
    mockReplSetGetStatusCmd();
}

vector<string> MockReplicaSet::getSecondaries() const {
    vector<string> secondaries;

    for (ReplSetConfig::MemberIterator member = _replConfig.membersBegin();
         member != _replConfig.membersEnd();
         ++member) {
        if (_primaryHost.empty() || member->getHostAndPort() != HostAndPort(_primaryHost)) {
            secondaries.push_back(member->getHostAndPort().toString());
        }
    }

    return secondaries;
}

MockRemoteDBServer* MockReplicaSet::getNode(const string& hostAndPort) {
    auto iter = _nodeMap.find(hostAndPort);
    return iter == _nodeMap.end() ? nullptr : iter->second;
}

const MockRemoteDBServer* MockReplicaSet::getNode(const string& hostAndPort) const {
    auto iter = _nodeMap.find(hostAndPort);
    return iter == _nodeMap.end() ? nullptr : iter->second;
}

repl::ReplSetConfig MockReplicaSet::getReplConfig() const {
    return _replConfig;
}

void MockReplicaSet::setConfig(const repl::ReplSetConfig& newConfig) {
    _replConfig = newConfig;
    mockHelloCmd();
    mockReplSetGetStatusCmd();
}

void MockReplicaSet::kill(const string& hostAndPort) {
    MONGO_verify(_nodeMap.count(hostAndPort) == 1);
    _nodeMap[hostAndPort]->shutdown();
}

void MockReplicaSet::kill(const vector<string>& hostList) {
    for (vector<string>::const_iterator iter = hostList.begin(); iter != hostList.end(); ++iter) {
        kill(*iter);
    }
}

void MockReplicaSet::restore(const string& hostAndPort) {
    MONGO_verify(_nodeMap.count(hostAndPort) == 1);
    _nodeMap[hostAndPort]->reboot();
}

BSONObj MockReplicaSet::mockHelloResponseFor(const MockRemoteDBServer& server) const {
    const auto hostAndPort = server.getServerHostAndPort();

    BSONObjBuilder builder;
    builder.append("setName", _setName);

    const MemberConfig* member = _replConfig.findMemberByHostAndPort(hostAndPort);
    if (!member) {
        builder.append("isWritablePrimary", false);
        builder.append("secondary", false);

        vector<string> hostList;
        builder.append("hosts", hostList);
    } else {
        const bool isPrimary = hostAndPort.toString() == getPrimary();
        builder.append("isWritablePrimary", isPrimary);
        builder.append("secondary", !isPrimary);

        {
            // TODO: add passives & arbiters
            vector<string> hostList;
            if (hasPrimary()) {
                hostList.push_back(getPrimary());
            }

            const vector<string> secondaries = getSecondaries();
            for (vector<string>::const_iterator secIter = secondaries.begin();
                 secIter != secondaries.end();
                 ++secIter) {
                hostList.push_back(*secIter);
            }

            builder.append("hosts", hostList);
        }

        if (hasPrimary()) {
            builder.append("primary", getPrimary());
        }

        if (member->isArbiter()) {
            builder.append("arbiterOnly", true);
        }

        if (member->getPriority() == 0 && !member->isArbiter()) {
            builder.append("passive", true);
        }

        if (member->getSecondaryDelay().count()) {
            builder.appendNumber("secondaryDelaySecs",
                                 durationCount<Seconds>(member->getSecondaryDelay()));
        }

        if (member->isHidden()) {
            builder.append("hidden", true);
        }

        if (!member->shouldBuildIndexes()) {
            builder.append("buildIndexes", false);
        }

        const ReplSetTagConfig tagConfig = _replConfig.getTagConfig();
        if (member->hasTags()) {
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

    builder.append("me", hostAndPort.toString());
    builder.append("ok", 1);

    return builder.obj();
}

void MockReplicaSet::mockHelloCmd() {
    for (ReplNodeMap::iterator nodeIter = _nodeMap.begin(); nodeIter != _nodeMap.end();
         ++nodeIter) {
        auto helloReply = mockHelloResponseFor(*nodeIter->second);

        nodeIter->second->setCommandReply("hello", helloReply);
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

        for (ReplSetConfig::MemberIterator member = _replConfig.membersBegin();
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

        std::sort(hostsField.begin(),
                  hostsField.end(),
                  SimpleBSONObjComparator::kInstance.makeLessThan());

        // TODO: syncingTo

        fullStatBuilder.append("set", _setName);
        fullStatBuilder.appendTimeT("date", time(nullptr));
        fullStatBuilder.append("myState", getState(node->getServerAddress()));
        fullStatBuilder.append("members", hostsField);
        fullStatBuilder.append("ok", true);

        node->setCommandReply("replSetGetStatus", fullStatBuilder.done());
    }
}

sdam::TopologyDescriptionPtr MockReplicaSet::getTopologyDescription(
    ClockSource* clockSource) const {
    sdam::TopologyDescriptionBuilder builder;

    // Note: MockReplicaSet::hasPrimary means there is a server being recognized as primary,
    // regardless of whether it is reachable. But for TopologyDescription, witPrimary also requires
    // that it has the hello response for the primary which we don't send out if primary is down.
    auto topologyType = sdam::TopologyType::kReplicaSetWithPrimary;
    if (!hasPrimary() || !getNode(getPrimary())->isRunning()) {
        topologyType = sdam::TopologyType::kReplicaSetNoPrimary;
    }

    builder.withSetName(_setName);
    builder.withTopologyType(topologyType);

    std::vector<sdam::ServerDescriptionPtr> servers;
    for (const auto& nodeEntry : _nodeMap) {
        const auto& server = *nodeEntry.second;
        if (server.isRunning()) {
            auto helloBSON = mockHelloResponseFor(server);
            sdam::HelloOutcome hello(server.getServerHostAndPort(), helloBSON);
            servers.push_back(std::make_shared<sdam::ServerDescription>(clockSource, hello));
        } else {
            sdam::HelloOutcome hello(server.getServerHostAndPort(), {}, "mock server unreachable");
            servers.push_back(std::make_shared<sdam::ServerDescription>(clockSource, hello));
        }
    }

    builder.withServers(servers);
    return builder.instance();
}

}  // namespace mongo
