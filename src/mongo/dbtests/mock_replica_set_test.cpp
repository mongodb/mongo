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

#include "mongo/client/dbclientinterface.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <set>
#include <string>

using mongo::BSONElement;
using mongo::BSONObj;
using mongo::BSONObjIterator;
using mongo::ConnectionString;
using mongo::MockRemoteDBServer;
using mongo::MockReplicaSet;

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
        ASSERT_EQUALS("n/$n0:27017,$n1:27017,$n2:27017",
                replSet.getConnectionString());
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
            }
            else {
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
            }
            else if (member["name"].str() == "$n0:27017") {
                ASSERT_EQUALS(1, member["state"].numberInt());
            }
            else {
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
            }
            else if (member["name"].str() == "$n0:27017") {
                ASSERT_EQUALS(1, member["state"].numberInt());
            }
            else {
                ASSERT_EQUALS(2, member["state"].numberInt());
            }
        }

        ASSERT(expectedMembers == memberList);
    }

    TEST(MockReplicaSetTest, IsMasterReconfigNodeRemoved) {
        MockReplicaSet replSet("n", 3);

        MockReplicaSet::ReplConfigMap config = replSet.getReplConfig();
        const string hostToRemove("$n1:27017");
        config.erase(hostToRemove);
        replSet.setConfig(config);

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

        MockReplicaSet::ReplConfigMap config = replSet.getReplConfig();
        const string hostToRemove("$n1:27017");
        config.erase(hostToRemove);
        replSet.setConfig(config);

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
                }
                else if (member["name"].str() == "$n0:27017") {
                    ASSERT_EQUALS(1, member["state"].numberInt());
                }
                else {
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
        for (vector<string>::const_iterator iter = secondaries.begin();
                iter != secondaries.end(); ++iter) {
            ASSERT(replSet.getNode(*iter)->isRunning());
        }
    }

    TEST(MockReplicaSetTest, KillMultipleNode) {
        MockReplicaSet replSet("n", 3);

        const vector<string> secondaries = replSet.getSecondaries();
        replSet.kill(replSet.getSecondaries());

        for (vector<string>::const_iterator iter = secondaries.begin();
                iter != secondaries.end(); ++iter) {
            ASSERT(!replSet.getNode(*iter)->isRunning());
        }

        const string priHostName(replSet.getPrimary());
        ASSERT(replSet.getNode(priHostName)->isRunning());
    }
}

