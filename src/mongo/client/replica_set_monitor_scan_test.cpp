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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor_test_fixture.h"

#include "mongo/client/mongo_uri.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using CoreScanTest = ReplicaSetMonitorTest;

TEST_F(CoreScanTest, CheckAllSeedsSerial) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, node->host.host() == "a");
        ASSERT(node->tags.isEmpty());
    }
}

TEST_F(CoreScanTest, CheckAllSeedsParallel) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));
    }

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, i == 0);
        ASSERT(node->tags.isEmpty());
    }
}

TEST_F(CoreScanTest, NoMasterInitAllUp) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << false << "secondary" << true << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }
}

TEST_F(CoreScanTest, MasterNotInSeeds_NoPrimaryInIsMaster) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << false << "secondary" << true << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c"
                                                      << "d")
                                        << "ok" << true));
    }

    // Only look at "d" after exhausting all other hosts
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT_EQUALS(ns.host.host(), "d");
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster" << true << "secondary" << false << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c"
                                                  << "d")
                                    << "ok" << true));


    ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }

    auto node = state->findNode(HostAndPort("d"));
    ASSERT(node);
    ASSERT_EQUALS(node->host.host(), "d");
    ASSERT(node->isUp);
    ASSERT_EQUALS(node->isMaster, true);
    ASSERT(node->tags.isEmpty());
}

TEST_F(CoreScanTest, MasterNotInSeeds_PrimaryInIsMaster) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "primary"
                                        << "d"
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c"
                                                      << "d")
                                        << "ok" << true));
    }

    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // validate final state
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(node->isUp);
        ASSERT_EQUALS(node->isMaster, false);
        ASSERT(node->tags.isEmpty());
    }

    auto node = state->findNode(HostAndPort("d"));
    ASSERT(node);
    ASSERT_EQUALS(node->host.host(), "d");
    ASSERT(node->isUp);
    ASSERT_EQUALS(node->isMaster, true);
    ASSERT(node->tags.isEmpty());
}

// Make sure we can use slaves we find even if we can't find a primary
TEST_F(CoreScanTest, SlavesUsableEvenIfNoMaster) {
    auto connStr = ConnectionString::forReplicaSet(kSetName, {HostAndPort("a")});
    auto state = makeState(MongoURI(connStr));
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
                                    << "ismaster" << false << "secondary" << true << "hosts"
                                    << BSON_ARRAY("a") << "ok" << true));

    // Check intended conditions for entry to getNextStep().
    ASSERT(state->currentScan->hostsToScan.empty());
    ASSERT(state->currentScan->waitingFor.empty());
    ASSERT(state->currentScan->possibleNodes == state->currentScan->triedHosts);
    ASSERT(state->getMatchingHost(secondary).empty());

    // getNextStep() should add the possible nodes to the replica set provisionally after being told
    // that there are no more hosts to contact. That is the final act of the scan.
    ASSERT_EQ(refresher.getNextStep().step, Refresher::NextStep::DONE);

    // Future calls should be able to return directly from the cached data.
    ASSERT(!state->getMatchingHost(secondary).empty());
}

// Test multiple nodes that claim to be master (we use a last-wins policy)
TEST_F(CoreScanTest, MultipleMasterLastNodeWins) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << true << "secondary" << false << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

        // Ensure the set primary is the host we just got a reply from
        HostAndPort currentPrimary = state->getMatchingHost(primaryOnly);
        ASSERT_EQUALS(currentPrimary.host(), basicSeeds[i].host());
        ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());

        // Check the state of each individual node
        for (size_t j = 0; j != basicSeeds.size(); ++j) {
            auto node = state->findNode(basicSeeds[j]);
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
TEST_F(CoreScanTest, MasterIsSourceOfTruth) {
    auto state = makeState(basicUri);
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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << (primary ? primaryHosts : secondaryHosts)
                                        << "ok" << true));

        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // Ensure that d is in the set but c is not
    ASSERT(state->findNode(HostAndPort("d")));
    ASSERT(!state->findNode(HostAndPort("c")));
}

// Test multiple master nodes that disagree about set membership
TEST_F(CoreScanTest, MultipleMastersDisagree) {
    auto state = makeState(basicUri);
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

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << true << "secondary" << false << "hosts"
                                        << hostsForSeed[i % 2] << "ok" << true));

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
                                    << "ismaster" << false << "secondary" << true << "hosts"
                                    << hostsForSeed[0] << "ok" << true));

    // scan should be complete
    ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());

    // Validate final state (only "c" should be master and "d" was added)
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size() + 1);

    auto nodes = state->nodes;
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
        const auto& node = *it;
        ASSERT(node.isUp);
        ASSERT_EQUALS(node.isMaster, node.host.host() == "c");
        ASSERT(seen.count(node.host));
    }
}

// Ensure getMatchingHost returns hosts even if scan is ongoing
TEST_F(CoreScanTest, GetMatchingDuringScan) {
    auto state = makeState(basicUri);
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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

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
TEST_F(CoreScanTest, OutOfBandFailedHost) {
    auto state = makeState(basicUri);
    ReplicaSetMonitorPtr rsm = std::make_shared<ReplicaSetMonitor>(state);
    Refresher refresher(state);

    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        NextStep ns = refresher.getNextStep();
    }

    for (size_t i = 0; i != basicSeeds.size(); ++i) {
        bool primary = (i == 0);

        refresher.receivedIsMaster(basicSeeds[i],
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

        if (i >= 1) {
            HostAndPort a("a");
            rsm->failedHost(a, {ErrorCodes::InternalError, "Test error"});
            auto node = state->findNode(a);
            ASSERT(node);
            ASSERT(!node->isUp);
            ASSERT(!node->isMaster);
        } else {
            auto node = state->findNode(HostAndPort("a"));
            ASSERT(node);
            ASSERT(node->isUp);
            ASSERT(node->isMaster);
        }
    }
}

// Newly elected primary with electionId >= maximum electionId seen by the Refresher
TEST_F(CoreScanTest, NewPrimaryWithMaxElectionId) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                   BSON(
                                       "setName"
                                       << "name"
                                       << "ismaster" << true << "secondary" << false << "hosts"
                                       << BSON_ARRAY("a"
                                                     << "b"
                                                     << "c")
                                       << "electionId"
                                       << OID::fromTerm(i)  // electionId must increase every cycle.
                                       << "ok" << true));

        // Ensure the set primary is the host we just got a reply from
        HostAndPort currentPrimary = state->getMatchingHost(primaryOnly);
        ASSERT_EQUALS(currentPrimary.host(), basicSeeds[i].host());
        ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());

        // Check the state of each individual node
        for (size_t j = 0; j != basicSeeds.size(); ++j) {
            auto node = state->findNode(basicSeeds[j]);
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
TEST_F(CoreScanTest, IgnoreElectionIdFromSecondaries) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "electionId"
                                        << (primary ? primaryElectionId : OID::gen()) << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));
    }

    // check that the SetState's maxElectionId == primary's electionId
    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);

    // Now all hosts have returned data
    NextStep ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    ASSERT(ns.host.empty());
}

// Stale Primary with obsolete electionId
TEST_F(CoreScanTest, StalePrimaryWithObsoleteElectionId) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    const OID firstElectionId = OID::gen();
    const OID secondElectionId = OID::gen();

    std::set<HostAndPort> seen;

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
                                        << "ismaster" << true << "secondary" << false
                                        << "setVersion" << 1 << "electionId" << secondElectionId
                                        << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

        auto node = state->findNode(ns.host);
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
                                        << "ismaster" << true << "secondary" << false
                                        << "electionId" << firstElectionId << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

        auto node = state->findNode(ns.host);
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
                                        << "ismaster" << false << "secondary" << true << "hosts"
                                        << BSON_ARRAY("a"
                                                      << "b"
                                                      << "c")
                                        << "ok" << true));

        auto node = state->findNode(ns.host);
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

TEST_F(CoreScanTest, NoPrimaryUpCheck) {
    auto state = makeState(basicUri);
    ReplicaSetMonitor rsm(state);
    ASSERT_FALSE(rsm.isKnownToHaveGoodPrimary());
}

TEST_F(CoreScanTest, PrimaryIsUpCheck) {
    auto state = makeState(basicUri);
    state->nodes.front().isMaster = true;
    ReplicaSetMonitor rsm(state);
    ASSERT_TRUE(rsm.isKnownToHaveGoodPrimary());
}

/**
 * Repl protocol verion 0 and 1 compatibility checking.
 */
TEST_F(CoreScanTest, TwoPrimaries2ndHasNewerConfigVersion) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    auto ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT(basicSeedsSet.count(ns.host));

    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster" << true << "secondary" << false << "setVersion"
                                    << 1 << "electionId" << OID("7fffffff0000000000000001")
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok" << true));

    // check that the SetState's maxElectionId == primary's electionId
    ASSERT_EQUALS(state->maxElectionId, OID("7fffffff0000000000000001"));
    ASSERT_EQUALS(state->configVersion, 1);

    const OID primaryElectionId = OID::gen();

    // Newer setVersion, no election id
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster" << true << "secondary" << false << "setVersion"
                                    << 2 << "electionId" << primaryElectionId << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok" << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);
}

/**
 * Repl protocol verion 0 and 1 compatibility checking.
 */
TEST_F(CoreScanTest, TwoPrimaries2ndHasOlderConfigVersion) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    auto ns = refresher.getNextStep();
    ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
    ASSERT(basicSeedsSet.count(ns.host));

    const OID primaryElectionId = OID::gen();
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster" << true << "secondary" << false << "electionId"
                                    << primaryElectionId << "setVersion" << 2 << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok" << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);

    // Older setVersion, but election id > previous election id. Newer setVersion should win.
    refresher.receivedIsMaster(ns.host,
                               -1,
                               BSON("setName"
                                    << "name"
                                    << "ismaster" << true << "secondary" << false << "setVersion"
                                    << 1 << "electionId" << OID("7fffffff0000000000000001")
                                    << "hosts"
                                    << BSON_ARRAY("a"
                                                  << "b"
                                                  << "c")
                                    << "ok" << true));

    ASSERT_EQUALS(state->maxElectionId, primaryElectionId);
    ASSERT_EQUALS(state->configVersion, 2);
}

using MaxStalenessMSTest = ReplicaSetMonitorTest;

/**
 * Success finding node matching maxStalenessMS parameter
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSMatch) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(100));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(10);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        bool nonStale = ns.host.host() == "c";
        nonStale |= primary;
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "lastWrite"
                                        << BSON("lastWriteDate" << (nonStale ? lastWriteDateNonStale
                                                                             : lastWriteDateStale)
                                                                << "opTime" << opTime)
                                        << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // make sure all secondaries are in the scan
    ASSERT(state->findNode(HostAndPort("b")));
    ASSERT(state->findNode(HostAndPort("c")));

    HostAndPort nonStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(nonStale.host(), "c");
}

/**
 * Fail matching maxStalenessMS parameter ( all secondary nodes are stale)
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSNoMatch) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "lastWrite"
                                        << BSON("lastWriteDate" << (primary ? lastWriteDateNonStale
                                                                            : lastWriteDateStale)
                                                                << "opTime" << opTime)
                                        << "ok" << true));

        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // make sure all secondaries are in the scan
    ASSERT(state->findNode(HostAndPort("b")));
    ASSERT(state->findNode(HostAndPort("c")));

    HostAndPort notFound = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notFound.host(), "");
}

/**
 * Success matching maxStalenessMS parameter when there is no primary node.
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSNoPrimaryMatch) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool isNonStale = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << false << "secondary" << true << "hosts"
                                        << hosts << "lastWrite"
                                        << BSON("lastWriteDate"
                                                << (isNonStale ? lastWriteDateNonStale
                                                               : lastWriteDateStale)
                                                << "opTime" << opTime)
                                        << "ok" << true));

        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // make sure all secondaries are in the scan
    ASSERT(state->findNode(HostAndPort("a")));
    ASSERT(state->findNode(HostAndPort("b")));
    ASSERT(state->findNode(HostAndPort("c")));

    HostAndPort notStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notStale.host(), "a");
}


/**
 * Fail matching maxStalenessMS parameter when all nodes are failed
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSAllFailed) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool isNonStale = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << false << "secondary" << true << "hosts"
                                        << hosts << "lastWrite"
                                        << BSON("lastWriteDate"
                                                << (isNonStale ? lastWriteDateNonStale
                                                               : lastWriteDateStale)
                                                << "opTime" << opTime)
                                        << "ok" << true));

        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // make sure all secondaries are in the scan
    refresher.failedHost(HostAndPort("a"), {ErrorCodes::InternalError, "Test error"});
    refresher.failedHost(HostAndPort("b"), {ErrorCodes::InternalError, "Test error"});
    refresher.failedHost(HostAndPort("c"), {ErrorCodes::InternalError, "Test error"});

    HostAndPort notStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notStale.host(), "");
}

/**
 * Fail matching maxStalenessMS parameter when all nodes except primary are failed
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSAllButPrimaryFailed) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "lastWrite"
                                        << BSON("lastWriteDate" << (primary ? lastWriteDateNonStale
                                                                            : lastWriteDateStale)
                                                                << "opTime" << opTime)
                                        << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    // make sure the primary is in the scan
    ASSERT(state->findNode(HostAndPort("a")));
    refresher.failedHost(HostAndPort("b"), {ErrorCodes::InternalError, "Test error"});
    refresher.failedHost(HostAndPort("c"), {ErrorCodes::InternalError, "Test error"});

    // No match because the request needs secondaryOnly host
    HostAndPort notStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notStale.host(), "");
}

/**
 * Fail matching maxStalenessMS parameter one secondary failed,  one secondary is stale
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSOneSecondaryFailed) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "lastWrite"
                                        << BSON("lastWriteDate" << (primary ? lastWriteDateNonStale
                                                                            : lastWriteDateStale)
                                                                << "opTime" << opTime)
                                        << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    ASSERT(state->findNode(HostAndPort("a")));
    ASSERT(state->findNode(HostAndPort("b")));
    refresher.failedHost(HostAndPort("c"), {ErrorCodes::InternalError, "Test error"});

    // No match because the write date is stale
    HostAndPort notStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notStale.host(), "");
}

/**
 * Success matching maxStalenessMS parameter when one secondary failed
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSNonStaleSecondaryMatched) {
    auto state = makeState(basicUri);
    Refresher refresher(state);
    repl::OpTime opTime{Timestamp{10, 10}, 10};

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);
    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        bool isNonStale = ns.host.host() == "b";
        refresher.receivedIsMaster(ns.host,
                                   -1,
                                   BSON("setName"
                                        << "name"
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "lastWrite"
                                        << BSON("lastWriteDate"
                                                << (isNonStale ? lastWriteDateNonStale
                                                               : lastWriteDateStale)
                                                << "opTime" << opTime)
                                        << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    refresher.failedHost(HostAndPort("a"), {ErrorCodes::InternalError, "Test error"});
    ASSERT(state->findNode(HostAndPort("b")));
    refresher.failedHost(HostAndPort("c"), {ErrorCodes::InternalError, "Test error"});

    HostAndPort notStale = state->getMatchingHost(secondary);
    ASSERT_EQUALS(notStale.host(), "b");
}

/**
 * Fail matching maxStalenessMS parameter when no lastWrite in the response
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSNoLastWrite) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    BSONArray hosts = BSON_ARRAY("a"
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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    ASSERT(state->findNode(HostAndPort("a")));
    ASSERT(state->findNode(HostAndPort("b")));
    ASSERT(state->findNode(HostAndPort("c")));

    ASSERT(state->getMatchingHost(secondary).empty());
}

/**
 * Match when maxStalenessMS=0 and no lastWrite in the response
 */
TEST_F(MaxStalenessMSTest, MaxStalenessMSZeroNoLastWrite) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    const ReadPreferenceSetting secondary(ReadPreference::SecondaryOnly, TagSet(), Seconds(0));
    BSONArray hosts = BSON_ARRAY("a"
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
                                        << "ismaster" << primary << "secondary" << !primary
                                        << "hosts" << hosts << "ok" << true));
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);

    ASSERT(state->findNode(HostAndPort("a")));
    ASSERT(state->findNode(HostAndPort("b")));
    ASSERT(state->findNode(HostAndPort("c")));

    ASSERT(!state->getMatchingHost(secondary).empty());
}

using MinOpTimeTest = ReplicaSetMonitorTest;
/**
 * Success matching minOpTime
 */
TEST_F(MinOpTimeTest, MinOpTimeMatched) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    repl::OpTime minOpTimeSetting{Timestamp{10, 10}, 10};
    repl::OpTime opTimeNonStale{Timestamp{10, 10}, 11};
    repl::OpTime opTimeStale{Timestamp{10, 10}, 9};

    ReadPreferenceSetting readPref(ReadPreference::Nearest, TagSet());
    readPref.minOpTime = minOpTimeSetting;
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        bool isNonStale = ns.host.host() == "b";
        BSONObj bson = BSON("setName"
                            << "name"
                            << "ismaster" << primary << "secondary" << !primary << "hosts" << hosts
                            << "lastWrite"
                            << BSON("opTime" << (isNonStale ? opTimeNonStale.toBSON()
                                                            : opTimeStale.toBSON()))
                            << "ok" << true);
        refresher.receivedIsMaster(ns.host, -1, bson);
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    HostAndPort notStale = state->getMatchingHost(readPref);
    ASSERT_EQUALS(notStale.host(), "b");
}

/**
 * Failure matching minOpTime on primary for SecondaryOnly
 */
TEST_F(MinOpTimeTest, MinOpTimeNotMatched) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    repl::OpTime minOpTimeSetting{Timestamp{10, 10}, 10};
    repl::OpTime opTimeNonStale{Timestamp{10, 10}, 11};
    repl::OpTime opTimeStale{Timestamp{10, 10}, 9};

    ReadPreferenceSetting readPref(ReadPreference::SecondaryOnly, TagSet());
    readPref.minOpTime = minOpTimeSetting;
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        bool isNonStale = ns.host.host() == "a";
        BSONObj bson = BSON("setName"
                            << "name"
                            << "ismaster" << primary << "secondary" << !primary << "hosts" << hosts
                            << "lastWrite"
                            << BSON("opTime" << (isNonStale ? opTimeNonStale.toBSON()
                                                            : opTimeStale.toBSON()))
                            << "ok" << true);
        refresher.receivedIsMaster(ns.host, -1, bson);
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    HostAndPort notStale = state->getMatchingHost(readPref);
    ASSERT(notStale.host() != "a");
}

/**
 * Ignore minOpTime if none is matched
 */
TEST_F(MinOpTimeTest, MinOpTimeIgnored) {
    auto state = makeState(basicUri);
    Refresher refresher(state);

    repl::OpTime minOpTimeSetting{Timestamp{10, 10}, 10};
    repl::OpTime opTimeStale{Timestamp{10, 10}, 9};

    Date_t lastWriteDateStale = Date_t::now() - Seconds(1000);
    Date_t lastWriteDateNonStale = Date_t::now() - Seconds(100);

    ReadPreferenceSetting readPref(ReadPreference::SecondaryOnly, TagSet(), Seconds(200));
    readPref.minOpTime = minOpTimeSetting;
    BSONArray hosts = BSON_ARRAY("a"
                                 << "b"
                                 << "c");

    // mock all replies
    NextStep ns = refresher.getNextStep();
    while (ns.step == NextStep::CONTACT_HOST) {
        bool primary = ns.host.host() == "a";
        bool isNonStale = ns.host.host() == "c";
        BSONObj bson = BSON("setName"
                            << "name"
                            << "ismaster" << primary << "secondary" << !primary << "hosts" << hosts
                            << "lastWrite"
                            << BSON("lastWriteDate"
                                    << (isNonStale || primary ? lastWriteDateNonStale
                                                              : lastWriteDateStale)
                                    << "opTime" << opTimeStale.toBSON())
                            << "ok" << true);
        refresher.receivedIsMaster(ns.host, -1, bson);
        ns = refresher.getNextStep();
    }

    // Ensure that we have heard from all hosts and scan is done
    ASSERT_EQUALS(ns.step, NextStep::DONE);
    HostAndPort notStale = state->getMatchingHost(readPref);
    ASSERT_EQUALS(notStale.host(), "c");
}

// -- ReplicaSetChangeNotifier/Listener tests --

class Listener : public ReplicaSetChangeNotifier::Listener {
public:
    void logEvent(StringData name, const Key& key) {
        log() << name << ": " << key;
    }
    void logEvent(StringData name, const State& state) {
        log() << name << ": "
              << "(" << state.generation << ") " << state.connStr << " | " << state.primary;
    }

    void onFoundSet(const Key& key) override {
        lastFoundSetId = ++eventId;
        logEvent("FoundSet", key);
    }
    void onPossibleSet(const State& state) override {
        lastPossibleSetId = ++eventId;
        logEvent("PossibleSet", state);
        lastState = state;
    }
    void onConfirmedSet(const State& state) override {
        lastConfirmedSetId = ++eventId;
        logEvent("ConfirmedSet", state);
        lastState = state;
    }
    void onDroppedSet(const Key& key) override {
        lastDroppedSetId = ++eventId;
        logEvent("DroppedSet", key);
    }

    int64_t eventId = -1;
    int64_t lastFoundSetId = -1;
    int64_t lastPossibleSetId = -1;
    int64_t lastConfirmedSetId = -1;
    int64_t lastDroppedSetId = -1;
    State lastState;
};

class ChangeNotifierTest : public ReplicaSetMonitorTest {
public:
    ChangeNotifierTest() = default;
    virtual ~ChangeNotifierTest() = default;

    enum class NodeState {
        kUnknown = 0,
        kPrimary,
        kSecondary,
        kStandalone,
    };

    auto& listener() const {
        return *static_cast<Listener*>(_listener.get());
    }

    void updateSet(std::map<HostAndPort, NodeState> replicaSet) {
        auto refresher = Refresher(_state);
        std::set<HostAndPort> seen;
        HostAndPort primary;
        std::set<HostAndPort> members;

        BSONArrayBuilder arrayBuilder;
        for (const auto& [host, nodeState] : replicaSet) {
            if (nodeState == NodeState::kStandalone) {
                continue;
            }

            if (nodeState == NodeState::kPrimary) {
                primary = host;
            }

            members.insert(host);

            arrayBuilder.append(StringData(host.host()));
        }
        auto bsonHosts = arrayBuilder.arr();

        auto markIsMaster = [&](auto host, bool isMaster) {
            refresher.receivedIsMaster(host,
                                       -1,
                                       BSON("setName" << kSetName << "ismaster" << isMaster
                                                      << "secondary" << !isMaster << "hosts"
                                                      << bsonHosts << "ok" << true));
        };

        auto markFailed = [&](auto host) {
            refresher.failedHost(host, {ErrorCodes::InternalError, "Test error"});
        };

        auto gen = listener().lastState.generation;

        NextStep ns = refresher.getNextStep();
        for (; ns.step != NextStep::DONE; ns = refresher.getNextStep()) {
            ASSERT_EQUALS(ns.step, NextStep::CONTACT_HOST);
            ASSERT(replicaSet.count(ns.host));
            ASSERT(!seen.count(ns.host));
            seen.insert(ns.host);

            // mock a reply
            switch (replicaSet[ns.host]) {
                case NodeState::kStandalone: {
                    markFailed(ns.host);
                } break;
                case NodeState::kPrimary: {
                    markIsMaster(ns.host, true);
                } break;
                case NodeState::kSecondary: {
                    markIsMaster(ns.host, false);
                } break;
                case NodeState::kUnknown:
                    MONGO_UNREACHABLE;
            };
        }

        // Verify that the listener received the right data
        if (gen != listener().lastState.generation) {
            // Our State is what the notifier thinks it should be
            ASSERT_EQUALS(listener().lastState.connStr,
                          listener().getCurrentState(kSetName.toString()).connStr);
            ASSERT_EQUALS(listener().lastState.primary,
                          listener().getCurrentState(kSetName.toString()).primary);
            ASSERT_EQUALS(listener().lastState.generation,
                          listener().getCurrentState(kSetName.toString()).generation);

            // Our State is what we'd expect
            ASSERT_EQUALS(listener().lastState.connStr.getSetName(), kSetName);
            ASSERT_EQUALS(listener().lastState.connStr.getServers().size(), members.size());
            ASSERT_EQUALS(listener().lastState.primary, primary);
        }

        ASSERT_EQUALS(ns.step, NextStep::DONE);
        ASSERT(ns.host.empty());
    }

protected:
    decltype(_notifier)::ListenerHandle _listener = _notifier.makeListener<Listener>();

    std::shared_ptr<SetState> _state = makeState(basicUri);
};

TEST_F(ChangeNotifierTest, NotifyNominal) {
    auto currentId = -1;

    // State exists. Signal: null
    ASSERT_EQ(listener().lastFoundSetId, currentId);

    // Initializing the state. Signal: FoundSet
    _state->init();
    ASSERT_EQ(listener().lastFoundSetId, ++currentId);

    // 'a' claims to be primary. Signal: Confirmed
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // Getting another scan with the same details. Signal: null
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().eventId, currentId);

    // Dropped. Signal: Dropped
    _state->drop();
    ASSERT_EQ(listener().lastDroppedSetId, ++currentId);
}

TEST_F(ChangeNotifierTest, NotifyElections) {
    auto currentId = -1;

    // State exists. Signal: null
    ASSERT_EQ(listener().lastFoundSetId, currentId);

    // Initializing the state. Signal: FoundSet
    _state->init();
    ASSERT_EQ(listener().lastFoundSetId, ++currentId);

    // 'a' claims to be primary. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // 'b' claims to be primary. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("b"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // All hosts tell us that they are not primary. Signal: null
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().eventId, currentId);

    // 'a' claims to be primary again. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // Dropped. Signal: Dropped
    _state->drop();
    ASSERT_EQ(listener().lastDroppedSetId, ++currentId);
}

TEST_F(ChangeNotifierTest, NotifyReconfig) {
    auto currentId = -1;

    // State exists. Signal: null
    ASSERT_EQ(listener().lastFoundSetId, currentId);

    // Initializing the state. Signal: FoundSet
    _state->init();
    ASSERT_EQ(listener().lastFoundSetId, ++currentId);

    // Update the set with a full scan showing no primary. Signal: PossibleSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().eventId, ++currentId);

    // Mark 'a' as removed. Signal: null
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kStandalone,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().eventId, currentId);

    // Discover 'd' as secondary. Signal: PossibleSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("b"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("d"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastPossibleSetId, ++currentId);

    // Mark 'b' as primary, no 'd'. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("b"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("d"),
            NodeState::kStandalone,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // Mark 'a' as removed. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("a"),
            NodeState::kStandalone,
        },
        {
            HostAndPort("b"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // Mark 'a' as secondary again. Signal: ConfirmedSet
    updateSet({
        {
            HostAndPort("b"),
            NodeState::kPrimary,
        },
        {
            HostAndPort("c"),
            NodeState::kSecondary,
        },
        {
            HostAndPort("a"),
            NodeState::kSecondary,
        },
    });
    ASSERT_EQ(listener().lastConfirmedSetId, ++currentId);

    // Dropped. Signal: Dropped
    _state->drop();
    ASSERT_EQ(listener().lastDroppedSetId, ++currentId);
}

}  // namespace
}  // namespace mongo
