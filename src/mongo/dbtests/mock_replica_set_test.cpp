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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <set>
#include <string>

using mongo::BSONArrayBuilder;
using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjBuilder;
using mongo::BSONObjIterator;
using mongo::ConnectionString;
using mongo::HostAndPort;
using mongo::MockRemoteDBServer;
using mongo::MockReplicaSet;
using mongo::repl::ReplicaSetConfig;

using std::set;
using std::string;
using std::vector;

namespace mongo_test {
TEST(MockReplicaSetTest, SetName) {
    MockReplicaSet replSet("n", 3);
    ASSERT_EQUALS("n", replSet.getSetName());
}

TEST(MockReplicaSetTest, ConnectionString) {
    MockReplicaSet replSet("n", 3);
    ASSERT_EQUALS("n/$n0:27017,$n1:27017,$n2:27017", replSet.getConnectionString());
}

TEST(MockReplicaSetTest, GetNode) {
    MockReplicaSet replSet("n", 3);
    ASSERT_EQUALS("$n0:27017", replSet.getNode("$n0:27017")->getServerAddress());
    ASSERT_EQUALS("$n1:27017", replSet.getNode("$n1:27017")->getServerAddress());
    ASSERT_EQUALS("$n2:27017", replSet.getNode("$n2:27017")->getServerAddress());
    ASSERT(replSet.getNode("$n3:27017") == NULL);
}

TEST(MockReplicaSetTest, IsMasterNode0) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedHosts;
    expectedHosts.insert("$n0:27017");
    expectedHosts.insert("$n1:27017");
    expectedHosts.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n0:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("ismaster" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT(cmdResponse["ismaster"].trueValue());
    ASSERT(!cmdResponse["secondary"].trueValue());
    ASSERT_EQUALS("$n0:27017", cmdResponse["me"].str());
    ASSERT_EQUALS("$n0:27017", cmdResponse["primary"].str());
    ASSERT_EQUALS("n", cmdResponse["setName"].str());

    set<string> hostList;
    BSONObjIterator iter(cmdResponse["hosts"].embeddedObject());
    while (iter.more()) {
        hostList.insert(iter.next().str());
    }

    ASSERT(expectedHosts == hostList);
}

TEST(MockReplicaSetTest, IsMasterNode1) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedHosts;
    expectedHosts.insert("$n0:27017");
    expectedHosts.insert("$n1:27017");
    expectedHosts.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n1:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("ismaster" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT(!cmdResponse["ismaster"].trueValue());
    ASSERT(cmdResponse["secondary"].trueValue());
    ASSERT_EQUALS("$n1:27017", cmdResponse["me"].str());
    ASSERT_EQUALS("$n0:27017", cmdResponse["primary"].str());
    ASSERT_EQUALS("n", cmdResponse["setName"].str());

    set<string> hostList;
    BSONObjIterator iter(cmdResponse["hosts"].embeddedObject());
    while (iter.more()) {
        hostList.insert(iter.next().str());
    }

    ASSERT(expectedHosts == hostList);
}

TEST(MockReplicaSetTest, IsMasterNode2) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedHosts;
    expectedHosts.insert("$n0:27017");
    expectedHosts.insert("$n1:27017");
    expectedHosts.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n2:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("ismaster" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT(!cmdResponse["ismaster"].trueValue());
    ASSERT(cmdResponse["secondary"].trueValue());
    ASSERT_EQUALS("$n2:27017", cmdResponse["me"].str());
    ASSERT_EQUALS("$n0:27017", cmdResponse["primary"].str());
    ASSERT_EQUALS("n", cmdResponse["setName"].str());

    set<string> hostList;
    BSONObjIterator iter(cmdResponse["hosts"].embeddedObject());
    while (iter.more()) {
        hostList.insert(iter.next().str());
    }

    ASSERT(expectedHosts == hostList);
}

TEST(MockReplicaSetTest, ReplSetGetStatusNode0) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedMembers;
    expectedMembers.insert("$n0:27017");
    expectedMembers.insert("$n1:27017");
    expectedMembers.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n0:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("replSetGetStatus" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT_EQUALS("n", cmdResponse["set"].str());
    ASSERT_EQUALS(1, cmdResponse["myState"].numberInt());

    set<string> memberList;
    BSONObjIterator iter(cmdResponse["members"].embeddedObject());
    while (iter.more()) {
        BSONElement member(iter.next());
        memberList.insert(member["name"].str());

        if (member["self"].trueValue()) {
            ASSERT_EQUALS(1, member["state"].numberInt());
            ASSERT_EQUALS("$n0:27017", member["name"].str());
        } else {
            ASSERT_EQUALS(2, member["state"].numberInt());
        }
    }

    ASSERT(expectedMembers == memberList);
}

TEST(MockReplicaSetTest, ReplSetGetStatusNode1) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedMembers;
    expectedMembers.insert("$n0:27017");
    expectedMembers.insert("$n1:27017");
    expectedMembers.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n1:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("replSetGetStatus" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT_EQUALS("n", cmdResponse["set"].str());
    ASSERT_EQUALS(2, cmdResponse["myState"].numberInt());

    set<string> memberList;
    BSONObjIterator iter(cmdResponse["members"].embeddedObject());
    while (iter.more()) {
        BSONElement member(iter.next());
        memberList.insert(member["name"].str());

        if (member["self"].trueValue()) {
            ASSERT_EQUALS(2, member["state"].numberInt());
            ASSERT_EQUALS("$n1:27017", member["name"].str());
        } else if (member["name"].str() == "$n0:27017") {
            ASSERT_EQUALS(1, member["state"].numberInt());
        } else {
            ASSERT_EQUALS(2, member["state"].numberInt());
        }
    }

    ASSERT(expectedMembers == memberList);
}

TEST(MockReplicaSetTest, ReplSetGetStatusNode2) {
    MockReplicaSet replSet("n", 3);
    set<string> expectedMembers;
    expectedMembers.insert("$n0:27017");
    expectedMembers.insert("$n1:27017");
    expectedMembers.insert("$n2:27017");

    BSONObj cmdResponse;
    MockRemoteDBServer* node = replSet.getNode("$n2:27017");
    const MockRemoteDBServer::InstanceID id = node->getInstanceID();
    bool ok = node->runCommand(id, "foo.bar", BSON("replSetGetStatus" << 1), cmdResponse);
    ASSERT(ok);

    ASSERT_EQUALS("n", cmdResponse["set"].str());
    ASSERT_EQUALS(2, cmdResponse["myState"].numberInt());

    set<string> memberList;
    BSONObjIterator iter(cmdResponse["members"].embeddedObject());
    while (iter.more()) {
        BSONElement member(iter.next());
        memberList.insert(member["name"].str());

        if (member["self"].trueValue()) {
            ASSERT_EQUALS(2, member["state"].numberInt());
            ASSERT_EQUALS("$n2:27017", member["name"].str());
        } else if (member["name"].str() == "$n0:27017") {
            ASSERT_EQUALS(1, member["state"].numberInt());
        } else {
            ASSERT_EQUALS(2, member["state"].numberInt());
        }
    }

    ASSERT(expectedMembers == memberList);
}

namespace {
/**
 * Takes a ReplicaSetConfig and a node to remove and returns a new config with equivalent
 * members minus the one specified to be removed.  NOTE: Does not copy over properties of the
 * members other than their id and host.
 */
ReplicaSetConfig _getConfigWithMemberRemoved(const ReplicaSetConfig& oldConfig,
                                             const HostAndPort& toRemove) {
    BSONObjBuilder newConfigBuilder;
    newConfigBuilder.append("_id", oldConfig.getReplSetName());
    newConfigBuilder.append("version", oldConfig.getConfigVersion());

    BSONArrayBuilder membersBuilder(newConfigBuilder.subarrayStart("members"));
    for (ReplicaSetConfig::MemberIterator member = oldConfig.membersBegin();
         member != oldConfig.membersEnd();
         ++member) {
        if (member->getHostAndPort() == toRemove) {
            continue;
        }

        membersBuilder.append(
            BSON("_id" << member->getId() << "host" << member->getHostAndPort().toString()));
    }

    membersBuilder.done();
    ReplicaSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(newConfigBuilder.obj()));
    ASSERT_OK(newConfig.validate());
    return newConfig;
}
}  // namespace

TEST(MockReplicaSetTest, IsMasterReconfigNodeRemoved) {
    MockReplicaSet replSet("n", 3);

    ReplicaSetConfig oldConfig = replSet.getReplConfig();
    const string hostToRemove("$n1:27017");
    ReplicaSetConfig newConfig = _getConfigWithMemberRemoved(oldConfig, HostAndPort(hostToRemove));
    replSet.setConfig(newConfig);

    {
        // Check isMaster for node still in set
        BSONObj cmdResponse;
        MockRemoteDBServer* node = replSet.getNode("$n0:27017");
        const MockRemoteDBServer::InstanceID id = node->getInstanceID();
        bool ok = node->runCommand(id, "foo.bar", BSON("ismaster" << 1), cmdResponse);
        ASSERT(ok);

        ASSERT(cmdResponse["ismaster"].trueValue());
        ASSERT(!cmdResponse["secondary"].trueValue());
        ASSERT_EQUALS("$n0:27017", cmdResponse["me"].str());
        ASSERT_EQUALS("$n0:27017", cmdResponse["primary"].str());
        ASSERT_EQUALS("n", cmdResponse["setName"].str());

        set<string> expectedHosts;
        expectedHosts.insert("$n0:27017");
        expectedHosts.insert("$n2:27017");

        set<string> hostList;
        BSONObjIterator iter(cmdResponse["hosts"].embeddedObject());
        while (iter.more()) {
            hostList.insert(iter.next().str());
        }

        ASSERT(expectedHosts == hostList);
        ASSERT(hostList.count(hostToRemove) == 0);
    }

    {
        // Check isMaster for node still not in set anymore
        BSONObj cmdResponse;
        MockRemoteDBServer* node = replSet.getNode(hostToRemove);
        const MockRemoteDBServer::InstanceID id = node->getInstanceID();
        bool ok = node->runCommand(id, "foo.bar", BSON("ismaster" << 1), cmdResponse);
        ASSERT(ok);

        ASSERT(!cmdResponse["ismaster"].trueValue());
        ASSERT(!cmdResponse["secondary"].trueValue());
        ASSERT_EQUALS(hostToRemove, cmdResponse["me"].str());
        ASSERT_EQUALS("n", cmdResponse["setName"].str());
    }
}

TEST(MockReplicaSetTest, replSetGetStatusReconfigNodeRemoved) {
    MockReplicaSet replSet("n", 3);

    ReplicaSetConfig oldConfig = replSet.getReplConfig();
    const string hostToRemove("$n1:27017");
    ReplicaSetConfig newConfig = _getConfigWithMemberRemoved(oldConfig, HostAndPort(hostToRemove));
    replSet.setConfig(newConfig);

    {
        // Check replSetGetStatus for node still in set
        BSONObj cmdResponse;
        MockRemoteDBServer* node = replSet.getNode("$n2:27017");
        const MockRemoteDBServer::InstanceID id = node->getInstanceID();
        bool ok = node->runCommand(id, "foo.bar", BSON("replSetGetStatus" << 1), cmdResponse);
        ASSERT(ok);

        ASSERT_EQUALS("n", cmdResponse["set"].str());
        ASSERT_EQUALS(2, cmdResponse["myState"].numberInt());

        set<string> memberList;
        BSONObjIterator iter(cmdResponse["members"].embeddedObject());
        while (iter.more()) {
            BSONElement member(iter.next());
            memberList.insert(member["name"].str());

            if (member["self"].trueValue()) {
                ASSERT_EQUALS(2, member["state"].numberInt());
                ASSERT_EQUALS("$n2:27017", member["name"].str());
            } else if (member["name"].str() == "$n0:27017") {
                ASSERT_EQUALS(1, member["state"].numberInt());
            } else {
                ASSERT_EQUALS(2, member["state"].numberInt());
            }
        }

        set<string> expectedMembers;
        expectedMembers.insert("$n0:27017");
        expectedMembers.insert("$n2:27017");
        ASSERT(expectedMembers == memberList);
    }

    {
        // Check replSetGetStatus for node still not in set anymore
        BSONObj cmdResponse;
        MockRemoteDBServer* node = replSet.getNode(hostToRemove);
        const MockRemoteDBServer::InstanceID id = node->getInstanceID();
        bool ok = node->runCommand(id, "foo.bar", BSON("replSetGetStatus" << 1), cmdResponse);
        ASSERT(ok);

        ASSERT_EQUALS("n", cmdResponse["set"].str());
        ASSERT_EQUALS(10, cmdResponse["myState"].numberInt());
    }
}

TEST(MockReplicaSetTest, KillNode) {
    MockReplicaSet replSet("n", 3);
    const string priHostName(replSet.getPrimary());
    replSet.kill(priHostName);

    ASSERT(!replSet.getNode(priHostName)->isRunning());

    const vector<string> secondaries = replSet.getSecondaries();
    for (vector<string>::const_iterator iter = secondaries.begin(); iter != secondaries.end();
         ++iter) {
        ASSERT(replSet.getNode(*iter)->isRunning());
    }
}

TEST(MockReplicaSetTest, KillMultipleNode) {
    MockReplicaSet replSet("n", 3);

    const vector<string> secondaries = replSet.getSecondaries();
    replSet.kill(replSet.getSecondaries());

    for (vector<string>::const_iterator iter = secondaries.begin(); iter != secondaries.end();
         ++iter) {
        ASSERT(!replSet.getNode(*iter)->isRunning());
    }

    const string priHostName(replSet.getPrimary());
    ASSERT(replSet.getNode(priHostName)->isRunning());
}
}
