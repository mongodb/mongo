/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

// Pull nested types to top-level scope
typedef ReplicaSetMonitor::IsMasterReply IsMasterReply;
typedef ReplicaSetMonitor::ScanState ScanState;
typedef ReplicaSetMonitor::ScanStatePtr ScanStatePtr;
typedef ReplicaSetMonitor::SetState SetState;
typedef ReplicaSetMonitor::SetStatePtr SetStatePtr;
typedef ReplicaSetMonitor::Refresher Refresher;
typedef Refresher::NextStep NextStep;
typedef ScanState::UnconfirmedReplies UnconfirmedReplies;
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

TEST(ReplicaSetMonitorTests, InitialState) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
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

TEST(ReplicaSetMonitorTests, CheckAllSeedsSerial) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
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
        refresher.receivedIsMaster(ns.host, -1, BSON(
                "setName" << "name"
             << "ismaster" << primary
             << "secondary" << !primary
             << "hosts" << BSON_ARRAY("a" << "b" << "c")
             << "ok" << true
             ));
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

TEST(ReplicaSetMonitorTests, CheckAllSeedsParallel) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
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
        refresher.receivedIsMaster(basicSeeds[i], -1, BSON(
                "setName" << "name"
             << "ismaster" << primary
             << "secondary" << !primary
             << "hosts" << BSON_ARRAY("a" << "b" << "c")
             << "ok" << true
             ));
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

TEST(ReplicaSetMonitorTests, NoMasterInitAllUp) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        refresher.receivedIsMaster(ns.host, -1, BSON(
                "setName" << "name"
             << "ismaster" << false
             << "secondary" << true
             << "hosts" << BSON_ARRAY("a" << "b" << "c")
             << "ok" << true
             ));
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

TEST(ReplicaSetMonitorTests, MasterNotInSeeds_NoPrimaryInIsMaster) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size(); i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        ASSERT(basicSeedsSet.count(ns.host));
        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        refresher.receivedIsMaster(ns.host, -1, BSON(
                "setName" << "name"
             << "ismaster" << false
             << "secondary" << true
             << "hosts" << BSON_ARRAY("a" << "b" << "c" << "d")
             << "ok" << true
             ));
    }

    // Only look at "d" after exhausting all other hosts
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "d");
    refresher.receivedIsMaster(ns.host, -1, BSON(
            "setName" << "name"
         << "ismaster" << true
         << "secondary" << false
         << "hosts" << BSON_ARRAY("a" << "b" << "c" << "d")
         << "ok" << true
         ));


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

TEST(ReplicaSetMonitorTests, MasterNotInSeeds_PrimaryInIsMaster) {
    SetStatePtr state = boost::make_shared<SetState>("name", basicSeedsSet);
    Refresher refresher(state);

    set<HostAndPort> seen;

    for (size_t i = 0; i < basicSeeds.size() + 1; i++) {
        NextStep ns = refresher.getNextStep();
        ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
        if (i == 1) // d should be the second host we contact since we are told it is primary
            ASSERT_EQUALS(ns.host.host(), "d");
        else
            ASSERT(basicSeedsSet.count(ns.host));

        ASSERT(!seen.count(ns.host));
        seen.insert(ns.host);

        // mock a reply
        bool primary = ns.host.host() == "d";
        refresher.receivedIsMaster(ns.host, -1, BSON(
                "setName" << "name"
             << "ismaster" << primary
             << "secondary" << !primary
             << "primary" << "d"
             << "hosts" << BSON_ARRAY("a" << "b" << "c" << "d")
             << "ok" << true
             ));
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
TEST(ReplicaSetMonitorTests, SlavesUsableEvenIfNoMaster) {
    std::set<HostAndPort> seeds;
    seeds.insert(HostAndPort("a"));
    SetStatePtr state = boost::make_shared<SetState>("name", seeds);
    Refresher refresher(state);

    const ReadPreferenceSetting secondary(ReadPreference_SecondaryOnly, TagSet());

    // Mock a reply from the only host we know about and have it claim to not be master or know
    // about any other hosts. This leaves the scan with no more hosts to scan, but all hosts are
    // still marked as down since we never contacted a master. The next call to
    // Refresher::getNextStep will apply all unconfimedReplies and return DONE.
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "a");
    refresher.receivedIsMaster(ns.host, -1, BSON(
            "setName" << "name"
         << "ismaster" << false
         << "secondary" << true
         << "hosts" << BSON_ARRAY("a")
         << "ok" << true
         ));

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

