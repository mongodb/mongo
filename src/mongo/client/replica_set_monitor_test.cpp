/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"


#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::set;

// Pull nested types to top-level scope
typedef ReplicaSetMonitor::IsMasterReply IsMasterReply;
typedef ReplicaSetMonitor::SetState SetState;
typedef ReplicaSetMonitor::SetStatePtr SetStatePtr;
typedef ReplicaSetMonitor::Refresher Refresher;
typedef Refresher::NextStep NextStep;
typedef SetState::Node Node;
typedef SetState::Nodes Nodes;

std::vector<HostAndPort> basicSeedsBuilder() {
    std::vector<HostAndPort> out;
    out.push_back(HostAndPort("a"));
    out.push_back(HostAndPort("b"));
    out.push_back(HostAndPort("c"));
    return out;
}

const std::vector<HostAndPort> basicSeeds = basicSeedsBuilder();
const std::set<HostAndPort> basicSeedsSet(basicSeeds.begin(), basicSeeds.end());

// NOTE: Unless stated otherwise, all tests assume exclusive access to state belongs to the
// current (only) thread, so they do not lock SetState::mutex before examining state. This is
// NOT something that non-test code should do.

TEST(ReplicaSetMonitor, InitialState) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    ASSERT_EQUALS(state->name, "name");
    ASSERT(state->seedNodes == basicSeedsSet);
    ASSERT(state->lastSeenMaster.empty());
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(!node->isUp);
        ASSERT(!node->isMaster);
        ASSERT(node->tags.isEmpty());
    }
}

TEST(ReplicaSetMonitor, IsMasterBadParse) {
    BSONObj ismaster = BSON("hosts" << BSON_ARRAY("mongo.example:badport"));
    IsMasterReply imr(HostAndPort("mongo.example:27017"), -1, ismaster);
    ASSERT_EQUALS(imr.ok, false);
}

TEST(ReplicaSetMonitor, IsMasterReplyRSNotInitiated) {
    BSONObj ismaster = BSON(
        "ismaster" << false << "secondary" << false << "info"
                   << "can't get local.system.replset config from self or any seed (EMPTYCONFIG)"
                   << "isreplicaset"
                   << true
                   << "maxBsonObjectSize"
                   << 16777216
                   << "maxMessageSizeBytes"
                   << 48000000
                   << "maxWriteBatchSize"
                   << 1000
                   << "localTime"
                   << mongo::jsTime()
                   << "maxWireVersion"
                   << 2
                   << "minWireVersion"
                   << 0
                   << "ok"
                   << 1);

    IsMasterReply imr(HostAndPort(), -1, ismaster);

    ASSERT_EQUALS(imr.ok, true);
    ASSERT_EQUALS(imr.setName, "");
    ASSERT_EQUALS(imr.hidden, false);
    ASSERT_EQUALS(imr.secondary, false);
    ASSERT_EQUALS(imr.isMaster, false);
    ASSERT_EQUALS(imr.configVersion, 0);
    ASSERT(!imr.electionId.isSet());
    ASSERT(imr.primary.empty());
    ASSERT(imr.normalHosts.empty());
    ASSERT(imr.tags.isEmpty());
}

TEST(ReplicaSetMonitor, IsMasterReplyRSPrimary) {
    BSONObj ismaster = BSON("setName"
                            << "test"
                            << "setVersion"
                            << 1
                            << "electionId"
                            << OID("7fffffff0000000000000001")
                            << "ismaster"
                            << true
                            << "secondary"
                            << false
                            << "hosts"
                            << BSON_ARRAY("mongo.example:3000")
                            << "primary"
                            << "mongo.example:3000"
                            << "me"
                            << "mongo.example:3000"
                            << "maxBsonObjectSize"
                            << 16777216
                            << "maxMessageSizeBytes"
                            << 48000000
                            << "maxWriteBatchSize"
                            << 1000
                            << "localTime"
                            << mongo::jsTime()
                            << "maxWireVersion"
                            << 2
                            << "minWireVersion"
                            << 0
                            << "ok"
                            << 1);

    IsMasterReply imr(HostAndPort("mongo.example:3000"), -1, ismaster);

    ASSERT_EQUALS(imr.ok, true);
    ASSERT_EQUALS(imr.host.toString(), HostAndPort("mongo.example:3000").toString());
    ASSERT_EQUALS(imr.setName, "test");
    ASSERT_EQUALS(imr.configVersion, 1);
    ASSERT_EQUALS(imr.electionId, OID("7fffffff0000000000000001"));
    ASSERT_EQUALS(imr.hidden, false);
    ASSERT_EQUALS(imr.secondary, false);
    ASSERT_EQUALS(imr.isMaster, true);
    ASSERT_EQUALS(imr.primary.toString(), HostAndPort("mongo.example:3000").toString());
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3000")));
    ASSERT(imr.tags.isEmpty());
}

TEST(ReplicaSetMonitor, IsMasterReplyPassiveSecondary) {
    BSONObj ismaster = BSON("setName"
                            << "test"
                            << "setVersion"
                            << 2
                            << "electionId"
                            << OID("7fffffff0000000000000001")
                            << "ismaster"
                            << false
                            << "secondary"
                            << true
                            << "hosts"
                            << BSON_ARRAY("mongo.example:3000")
                            << "passives"
                            << BSON_ARRAY("mongo.example:3001")
                            << "primary"
                            << "mongo.example:3000"
                            << "passive"
                            << true
                            << "me"
                            << "mongo.example:3001"
                            << "maxBsonObjectSize"
                            << 16777216
                            << "maxMessageSizeBytes"
                            << 48000000
                            << "maxWriteBatchSize"
                            << 1000
                            << "localTime"
                            << mongo::jsTime()
                            << "maxWireVersion"
                            << 2
                            << "minWireVersion"
                            << 0
                            << "ok"
                            << 1);

    IsMasterReply imr(HostAndPort("mongo.example:3001"), -1, ismaster);

    ASSERT_EQUALS(imr.ok, true);
    ASSERT_EQUALS(imr.host.toString(), HostAndPort("mongo.example:3001").toString());
    ASSERT_EQUALS(imr.setName, "test");
    ASSERT_EQUALS(imr.configVersion, 2);
    ASSERT_EQUALS(imr.hidden, false);
    ASSERT_EQUALS(imr.secondary, true);
    ASSERT_EQUALS(imr.isMaster, false);
    ASSERT_EQUALS(imr.primary.toString(), HostAndPort("mongo.example:3000").toString());
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3000")));
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3001")));
    ASSERT(imr.tags.isEmpty());
    ASSERT(!imr.electionId.isSet());
}

TEST(ReplicaSetMonitor, IsMasterReplyHiddenSecondary) {
    BSONObj ismaster = BSON("setName"
                            << "test"
                            << "setVersion"
                            << 2
                            << "electionId"
                            << OID("7fffffff0000000000000001")
                            << "ismaster"
                            << false
                            << "secondary"
                            << true
                            << "hosts"
                            << BSON_ARRAY("mongo.example:3000")
                            << "primary"
                            << "mongo.example:3000"
                            << "passive"
                            << true
                            << "hidden"
                            << true
                            << "me"
                            << "mongo.example:3001"
                            << "maxBsonObjectSize"
                            << 16777216
                            << "maxMessageSizeBytes"
                            << 48000000
                            << "maxWriteBatchSize"
                            << 1000
                            << "localTime"
                            << mongo::jsTime()
                            << "maxWireVersion"
                            << 2
                            << "minWireVersion"
                            << 0
                            << "ok"
                            << 1);

    IsMasterReply imr(HostAndPort("mongo.example:3001"), -1, ismaster);

    ASSERT_EQUALS(imr.ok, true);
    ASSERT_EQUALS(imr.host.toString(), HostAndPort("mongo.example:3001").toString());
    ASSERT_EQUALS(imr.setName, "test");
    ASSERT_EQUALS(imr.configVersion, 2);
    ASSERT_EQUALS(imr.hidden, true);
    ASSERT_EQUALS(imr.secondary, true);
    ASSERT_EQUALS(imr.isMaster, false);
    ASSERT_EQUALS(imr.primary.toString(), HostAndPort("mongo.example:3000").toString());
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3000")));
    ASSERT(imr.tags.isEmpty());
    ASSERT(!imr.electionId.isSet());
}

TEST(ReplicaSetMonitor, IsMasterSecondaryWithTags) {
    BSONObj ismaster = BSON("setName"
                            << "test"
                            << "setVersion"
                            << 2
                            << "electionId"
                            << OID("7fffffff0000000000000001")
                            << "ismaster"
                            << false
                            << "secondary"
                            << true
                            << "hosts"
                            << BSON_ARRAY("mongo.example:3000"
                                          << "mongo.example:3001")
                            << "primary"
                            << "mongo.example:3000"
                            << "me"
                            << "mongo.example:3001"
                            << "maxBsonObjectSize"
                            << 16777216
                            << "maxMessageSizeBytes"
                            << 48000000
                            << "maxWriteBatchSize"
                            << 1000
                            << "localTime"
                            << mongo::jsTime()
                            << "maxWireVersion"
                            << 2
                            << "minWireVersion"
                            << 0
                            << "tags"
                            << BSON("dc"
                                    << "nyc"
                                    << "use"
                                    << "production")
                            << "ok"
                            << 1);

    IsMasterReply imr(HostAndPort("mongo.example:3001"), -1, ismaster);

    ASSERT_EQUALS(imr.ok, true);
    ASSERT_EQUALS(imr.host.toString(), HostAndPort("mongo.example:3001").toString());
    ASSERT_EQUALS(imr.setName, "test");
    ASSERT_EQUALS(imr.configVersion, 2);
    ASSERT_EQUALS(imr.hidden, false);
    ASSERT_EQUALS(imr.secondary, true);
    ASSERT_EQUALS(imr.isMaster, false);
    ASSERT_EQUALS(imr.primary.toString(), HostAndPort("mongo.example:3000").toString());
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3000")));
    ASSERT(imr.normalHosts.count(HostAndPort("mongo.example:3001")));
    ASSERT(imr.tags.hasElement("dc"));
    ASSERT(imr.tags.hasElement("use"));
    ASSERT(!imr.electionId.isSet());
    ASSERT_EQUALS(imr.tags["dc"].str(), "nyc");
    ASSERT_EQUALS(imr.tags["use"].str(), "production");
}

TEST(ReplicaSetMonitor, CheckAllSeedsSerial) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, node->host.host() == "a");
        ASSERT(node->tags.isEmpty());
    }
}

TEST(ReplicaSetMonitor, CheckAllSeedsParallel) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    // get all hosts to contact first
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);
    }


    // mock all replies
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        // All hosts to talk to are already dispatched, but no reply has been received
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::WAIT);
        ASSERT(ns.host.empty());

        bool primary = i == 0;
        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));
    }

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, i == 0);
        ASSERT(node->tags.isEmpty());
    }
}

TEST(ReplicaSetMonitor, NoMasterInitAllUp) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << false
                                        << "secondary"
                                        << true
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }
}

TEST(ReplicaSetMonitor, MasterNotInSeeds_NoPrimaryInIsMaster) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << false
                                        << "secondary"
                                        << true
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c"
                                                      << "d")
                                        << "ok"
                                        << true));
    }

    // Only look at "d" after exhausting all other hosts
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "d");
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << true
                                    << "secondary"
                                    << false
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c"
                                                  << "d")
                                    << "ok"
                                    << true));


    ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }

    Node* node = state->findNode(HostAndPort("d"));
    ASSERT(node);
    ASSERT_EQUALS(node->host.host(), "d");
    ASSERT(node->isUp);
    ASSERT_EQUALS(node->isMaster, true);
    ASSERT(node->tags.isEmpty());
}

TEST(ReplicaSetMonitor, MasterNotInSeeds_PrimaryInIsMaster) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size() + 1; i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        if (i == 1) {  // d should be the second host we contact since we are told it is primary
            ASSERT_EQUALS(ns.host.host(), "d");
        } else {
            ASSERT(basicSeedsSet.count(ns.host));
        }

        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        bool primary = ns.host.host() == "d";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "primary"
                                        << "d"
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c"
                                                      << "d")
                                        << "ok"
                                        << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        Node* node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }

    Node* node = state->findNode(HostAndPort("d"));
    ASSERT(node);
    ASSERT_EQUALS(node->host.host(), "d");
    ASSERT(node->isUp);
    ASSERT_EQUALS(node->isMaster, true);
    ASSERT(node->tags.isEmpty());
}

// Make sure we can use slaves we find even if we can't find a primary
TEST(ReplicaSetMonitor, SlavesUsableEvenIfNoMaster) {
    std::set<HostAndPort> seeds;
    seeds.insert(HostAndPort("a"));
    SetStatePtr state = std::make_shared<SetState>("name", seeds);
    Refresher refresher(state);

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet());

    // Mock a reply from the only host we know about and have it claim to not be master or know
    // about any other hosts. This leaves the scan with no more hosts to scan, but all hosts are
    // still marked as down since we never contacted a master. The next call to
    // Refresher::getNextStep will apply all unconfimedReplies and return DONE.
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "a");
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << false
                                    << "secondary"
                                    << true
                                    << "hosts"
                                    << BSON_ARRAY("a")
                                    << "ok"
                                    << true));

    // Check intended conditions for entry to refreshUntilMatches.
    ASSERT(state->currentScan->hostsToScan.empty());
    ASSERT(state->currentScan->waitingFor.empty());
    ASSERT(state->currentScan->possibleNodes == state->currentScan->triedHosts);
    ASSERT(state->getMatchingHost(secondary).empty());

    // This calls getNextStep after not finding a matching host. We want to ensure that it checks
    // again after being told that there are no more hosts to contact.
    ASSERT(!refresher.refreshUntilMatches(secondary).empty());

    // Future calls should be able to return directly from the cached data.
    ASSERT(!state->getMatchingHost(secondary).empty());
}

// Test multiple nodes that claim to be master (we use a last-wins policy)
TEST(ReplicaSetMonitor, MultipleMasterLastNodeWins) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    // get all hosts to contact first
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);
    }

    const ReadPreferenceSetting primaryOnly(ReadPreference::PrimaryOnly, TagSet());

    // mock all replies
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        // All hosts to talk to are already dispatched, but no reply has been received
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::WAIT);
        ASSERT(ns.host.empty());

        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << true
                                        << "secondary"
                                        << false
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        // Ensure the set primary is the host we just got a reply from
        HostAndPort currentPrimary = state->getMatchingHost(primaryOnly);
        ASSERT_EQUALS(currentPrimary.host(), basicSeeds[i].host());
        ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());

        // Check the state of each individual node
        for (size_t j = 0; j != basicSeeds.size(); ++j) {
            Node* node = state->findNode(basicSeeds[j]);
            ASSERT(node);
            ASSERT_EQUALS(node->host.toString(), basicSeeds[j].toString());
            ASSERT_EQUALS(node->isUp, j <= i);
            ASSERT_EQUALS(node->isMaster, j == i);
            ASSERT(node->tags.isEmpty());
        }
    }

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

// Test nodes disagree about who is in the set, master is source of truth
TEST(ReplicaSetMonitor, MasterIsSourceOfTruth) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    BSONArray primaryHosts = BSON_ARRAY("a"
                                        << "b"
                                        << "d");
    BSONArray secondaryHosts = BSON_ARRAY("a"
                                          << "b"
                                          << "c");

    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "hosts"
                                        << (primary ? primaryHosts : secondaryHosts)
                                        << "ok"
                                        << true));

        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // Ensure that d is in the set but c is not
    ASSERT(state->findNode(HostAndPort("d")));
    ASSERT(!state->findNode(HostAndPort("c")));
}

// Test multiple master nodes that disagree about set membership
TEST(ReplicaSetMonitor, MultipleMastersDisagree) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    BSONArray hostsForSeed[3];
    hostsForSeed[0] = BSON_ARRAY("a"
                                 << "b"
                                 << "c"
                                 << "d");
    hostsForSeed[1] = BSON_ARRAY("a"
                                 << "b"
                                 << "c"
                                 << "e");
    hostsForSeed[2] = hostsForSeed[0];

    set<HostAndPort> seen;

    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);
    }

    const ReadPreferenceSetting primaryOnly(ReadPreference::PrimaryOnly, TagSet());

    // mock all replies
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << true
                                        << "secondary"
                                        << false
                                        << "hosts"
                                        << hostsForSeed[i % 2]
                                        << "ok"
                                        << true));

        // Ensure the primary is the host we just got a reply from
        HostAndPort currentPrimary = state->getMatchingHost(primaryOnly);
        ASSERT_EQUALS(currentPrimary.host(), basicSeeds[i].host());

        // Ensure each primary discovered becomes source of truth
        if (i == 1) {
            // "b" thinks node "e" is a member but "d" is not
            ASSERT(state->findNode(HostAndPort("e")));
            ASSERT(!state->findNode(HostAndPort("d")));
        } else {
            // "a" and "c" think node "d" is a member but "e" is not
            ASSERT(state->findNode(HostAndPort("d")));
            ASSERT(!state->findNode(HostAndPort("e")));
        }
    }

    // next step should be to contact "d"
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "d");
    seen.insert(ns.host);

    // reply from "d"
    refresher.receivedIsMaster(HostAndPort("d"),
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << false
                                    << "secondary"
                                    << true
                                    << "hosts"
                                    << hostsForSeed[0]
                                    << "ok"
                                    << true));

    // scan should be complete
    ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // Validate final state (only "c" should be master and "d" was added)
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);

    std::vector<Node> nodes = state->nodes;
    for (std::vector<Node>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        const Node& node = *it;
        ASSERT(node.isUp);
        ASSERT_EQUALS(node.isMaster, node.host.host() == "c");
        ASSERT(seen.count(node.host));
    }
}

// Ensure getMatchingHost returns hosts even if scan is ongoing
TEST(ReplicaSetMonitor, GetMatchingDuringScan) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    const ReadPreferenceSetting primaryOnly(ReadPreference::PrimaryOnly, TagSet());
    const ReadPreferenceSetting secondaryOnly(ReadPreference::SecondaryOnly, TagSet());

    for (std::vector<HostAndPort>::const_iterator it = basicSeeds.begin(); it != basicSeeds.end();
         ++it) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(state->getMatchingHost(primaryOnly).empty());
        ASSERT(state->getMatchingHost(secondaryOnly).empty());
    }

    // mock replies and validate set state as replies come back
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::WAIT);
        ASSERT(ns.host.empty());

        bool primary = (i == 1);
        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        bool hasPrimary = !(state->getMatchingHost(primaryOnly).empty());
        bool hasSecondary = !(state->getMatchingHost(secondaryOnly).empty());

        // secondary node has not been confirmed by primary until i == 1
        if (i >= 1) {
            ASSERT(hasPrimary);
            ASSERT(hasSecondary);
        } else {
            ASSERT(!hasPrimary);
            ASSERT(!hasSecondary);
        }
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

// Ensure nothing breaks when out-of-band failedHost is called during scan
TEST(ReplicaSetMonitor, OutOfBandFailedHost) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    ReplicaSetMonitorPtr rsm = std::make_shared<ReplicaSetMonitor>(state);
    Refresher refresher = rsm->startOrContinueRefresh();

    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
    }

    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        bool primary = (i == 0);

        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        if (i >= 1) {
            HostAndPort a("a");
            rsm->failedHost(a);
            Node* node = state->findNode(a);
            ASSERT(node);
            ASSERT(!node->isUp);
            ASSERT(!node->isMaster);
        } else {
            Node* node = state->findNode(HostAndPort("a"));
            ASSERT(node);
            ASSERT(node->isUp);
            ASSERT(node->isMaster);
        }
    }
}

// Newly elected primary with electionId >= maximum electionId seen by the Refresher
TEST(ReplicaSetMonitorTests, NewPrimaryWithMaxElectionId) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    // get all hosts to contact first
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);
    }

    const ReadPreferenceSetting primaryOnly(ReadPreference::PrimaryOnly, TagSet());

    // mock all replies
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        // All hosts to talk to are already dispatched, but no reply has been received
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::WAIT);
        ASSERT(ns.host.empty());

        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << true
                                        << "secondary"
                                        << false
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "electionId"
                                        << OID::gen()
                                        << "ok"
                                        << true));

        // Ensure the set primary is the host we just got a reply from
        HostAndPort currentPrimary = state->getMatchingHost(primaryOnly);
        ASSERT_EQUALS(currentPrimary.host(), basicSeeds[i].host());
        ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());

        // Check the state of each individual node
        for (size_t j = 0; j != basicSeeds.size(); ++j) {
            Node* node = state->findNode(basicSeeds[j]);
            ASSERT(node);
            ASSERT_EQUALS(node->host.toString(), basicSeeds[j].toString());
            ASSERT_EQUALS(node->isUp, j <= i);
            ASSERT_EQUALS(node->isMaster, j == i);
            ASSERT(node->tags.isEmpty());
        }
    }

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

// Ignore electionId of secondaries
TEST(ReplicaSetMonitorTests, IgnoreElectionIdFromSecondaries) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    const OID primaryElectionId = OID::gen();

    // mock all replies
    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        const bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << primary
                                        << "secondary"
                                        << !primary
                                        << "electionId"
                                        << (primary ? primaryElectionId : OID::gen())
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));
    }

    // check that the SetState's maxElectionId == primary's electionId
    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

// Stale Primary with obsolete electionId
TEST(ReplicaSetMonitorTests, StalePrimaryWithObsoleteElectionId) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    const OID firstElectionId = OID::gen();
    const OID secondElectionId = OID::gen();

    set<HostAndPort> seen;

    // contact first host claiming to be primary with greater electionId
    {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << true
                                        << "secondary"
                                        << false
                                        << "setVersion"
                                        << 1
                                        << "electionId"
                                        << secondElectionId
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        Node* node = state->findNode(ns.host);
        ASSERT(node);
        ASSERT_TRUE(node->isMaster);
        ASSERT_EQUALS(state->maxElectionId, secondElectionId);
    }

    // contact second host claiming to be primary with smaller electionId
    {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << true
                                        << "secondary"
                                        << false
                                        << "electionId"
                                        << firstElectionId
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        Node* node = state->findNode(ns.host);
        ASSERT(node);
        // The SetState shouldn't see this host as master
        ASSERT_FALSE(node->isMaster);
        // the max electionId should remain the same
        ASSERT_EQUALS(state->maxElectionId, secondElectionId);
    }

    // third host is a secondary
    {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster"
                                        << false
                                        << "secondary"
                                        << true
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok"
                                        << true));

        Node* node = state->findNode(ns.host);
        ASSERT(node);
        ASSERT_FALSE(node->isMaster);
        // the max electionId should remain the same
        ASSERT_EQUALS(state->maxElectionId, secondElectionId);
    }

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

TEST(ReplicaSetMonitor, NoPrimaryUpCheck) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    ReplicaSetMonitor rsm(state);
    ASSERT_FALSE(rsm.isKnownToHaveGoodPrimary());
}

TEST(ReplicaSetMonitor, PrimaryIsUpCheck) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    state->nodes.front().isMaster = true;
    ReplicaSetMonitor rsm(state);
    ASSERT_TRUE(rsm.isKnownToHaveGoodPrimary());
}

/**
 * Repl protocol verion 0 and 1 compatibility checking.
 */
TEST(ReplicaSetMonitorTests, TwoPrimaries2ndHasNewerConfigVersion) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    auto ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT(basicSeedsSet.count(ns.host));

    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << true
                                    << "secondary"
                                    << false
                                    << "setVersion"
                                    << 1
                                    << "electionId"
                                    << OID("7fffffff0000000000000001")
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok"
                                    << true));

    // check that the SetState's maxElectionId == primary's electionId
    ASSERT_EQUALS(state->maxElectionId, OID("7fffffff0000000000000001"));
    ASSERT_EQUALS(state->configVersion, 1);

    const OID primaryElectionId = OID::gen();

    // Newer setVersion, no election id
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << true
                                    << "secondary"
                                    << false
                                    << "setVersion"
                                    << 2
                                    << "electionId"
                                    << primaryElectionId
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok"
                                    << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);
}

/**
 * Repl protocol verion 0 and 1 compatibility checking.
 */
TEST(ReplicaSetMonitorTests, TwoPrimaries2ndHasOlderConfigVersion) {
    SetStatePtr state = std::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    auto ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT(basicSeedsSet.count(ns.host));

    const OID primaryElectionId = OID::gen();
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << true
                                    << "secondary"
                                    << false
                                    << "electionId"
                                    << primaryElectionId
                                    << "setVersion"
                                    << 2
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok"
                                    << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);

    // Older setVersion, but election id > previous election id. Newer setVersion should win.
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster"
                                    << true
                                    << "secondary"
                                    << false
                                    << "setVersion"
                                    << 1
                                    << "electionId"
                                    << OID("7fffffff0000000000000001")
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok"
                                    << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);
}

}  // namespace
