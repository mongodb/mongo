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
 */

/**
 * This file contains test for ReplicaSetMonitor::Node,
 * ReplicaSetMonitor::selectNode and TagSet
 */

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/unittest/unittest.h"

#include <vector>

using std::map;
using std::vector;
using std::string;
using boost::scoped_ptr;

using mongo::BSONObj;
using mongo::BSONObjBuilder;
using mongo::BSONArray;
using mongo::BSONArrayBuilder;
using mongo::BSONElement;
using mongo::ConnectionString;
using mongo::HostAndPort;
using mongo::MockReplicaSet;
using mongo::ReadPreference;
using mongo::ReplicaSetMonitor;
using mongo::ReplicaSetMonitorPtr;
using mongo::ScopedDbConnection;
using mongo::TagSet;

namespace mongo_test {
    const BSONObj SampleIsMasterDoc = BSON("tags"
                                            << BSON("dc" << "NYC"
                                                    << "p" << "2"
                                                    << "region" << "NA"));
    const BSONObj NoTagIsMasterDoc = BSON("isMaster" << true);

    TEST(ReplSetMonitorNode, SimpleGoodMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = BSON("tags" << BSON("dc" << "sf"));
        ASSERT(node.matchesTag(BSON("dc" << "sf")));
    }

    TEST(ReplSetMonitorNode, SimpleBadMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = BSON("tags" << BSON("dc" << "nyc"));
        ASSERT(!node.matchesTag(BSON("dc" << "sf")));
    }

    TEST(ReplSetMonitorNode, ExactMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(node.matchesTag(SampleIsMasterDoc["tags"].Obj()));
    }

    TEST(ReplSetMonitorNode, EmptyTag) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(node.matchesTag(BSONObj()));
    }

    TEST(ReplSetMonitorNode, MemberNoTagMatchesEmptyTag) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = NoTagIsMasterDoc;
        ASSERT(node.matchesTag(BSONObj()));
    }

    TEST(ReplSetMonitorNode, MemberNoTagDoesNotMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = NoTagIsMasterDoc.copy();
        ASSERT(!node.matchesTag(BSON("dc" << "NYC")));
    }

    TEST(ReplSetMonitorNode, IncompleteMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(!node.matchesTag(BSON("dc" << "NYC"
                                     << "p" << "2"
                                     << "hello" << "world")));
    }

    TEST(ReplSetMonitorNode, PartialMatch) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(node.matchesTag(BSON("dc" << "NYC"
                                    << "p" << "2")));
    }

    TEST(ReplSetMonitorNode, SingleTagCrit) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(node.matchesTag(BSON("p" << "2")));
    }

    TEST(ReplSetMonitorNode, BadSingleTagCrit) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(!node.matchesTag(BSON("dc" << "SF")));
    }

    TEST(ReplSetMonitorNode, NonExistingFieldTag) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(!node.matchesTag(BSON("noSQL" << "Mongo")));
    }

    TEST(ReplSetMonitorNode, UnorederedMatching) {
            ReplicaSetMonitor::Node node(HostAndPort(), NULL);
            node.lastIsMaster = SampleIsMasterDoc.copy();
            ASSERT(node.matchesTag(BSON("p" << "2" << "dc" << "NYC")));
    }

    TEST(ReplSetMonitorNode, SameValueDiffKey) {
        ReplicaSetMonitor::Node node(HostAndPort(), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();
        ASSERT(!node.matchesTag(BSON("datacenter" << "NYC")));
    }

    TEST(ReplSetMonitorNode, SimpleToString) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        // Should not throw any exceptions
        ASSERT(!node.toString().empty());
    }

    TEST(ReplSetMonitorNode, SimpleToStringWithNoTag)  {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = NoTagIsMasterDoc.copy();

        // Should not throw any exceptions
        ASSERT(!node.toString().empty());
    }

    TEST(ReplSetMonitorNode, PriNodeCompatibleTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = true;
        node.secondary = false;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "NYC"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, SecNodeCompatibleTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = false;
        node.secondary = true;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "NYC"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, PriNodeNotCompatibleTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = true;
        node.secondary = false;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "SF"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, SecNodeNotCompatibleTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = false;
        node.secondary = true;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "SF"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, PriNodeCompatiblMultiTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = true;
        node.secondary = false;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "RP"));
        builder.append(BSON("dc" << "NYC" << "p" << "2"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, SecNodeCompatibleMultiTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = false;
        node.secondary = true;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "RP"));
        builder.append(BSON("dc" << "NYC" << "p" << "2"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, PriNodeNotCompatibleMultiTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = true;
        node.secondary = false;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "sf"));
        builder.append(BSON("dc" << "NYC" << "P" << "4"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    TEST(ReplSetMonitorNode, SecNodeNotCompatibleMultiTag) {
        ReplicaSetMonitor::Node node(HostAndPort("dummy", 3), NULL);
        node.lastIsMaster = SampleIsMasterDoc.copy();

        node.ok = true;
        node.ismaster = false;
        node.secondary = true;

        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "sf"));
        builder.append(BSON("dc" << "NYC" << "P" << "4"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_PrimaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryPreferred, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_SecondaryOnly, &tags));
        ASSERT(!node.isCompatible(mongo::ReadPreference_Nearest, &tags));
    }

    class NodeSetFixtures {
    public:
        static vector<ReplicaSetMonitor::Node> getThreeMemberWithTags();
    };

    vector<ReplicaSetMonitor::Node> NodeSetFixtures::getThreeMemberWithTags() {
        vector<ReplicaSetMonitor::Node> nodes;

        nodes.push_back(ReplicaSetMonitor::Node(HostAndPort("a"), NULL));
        nodes.push_back(ReplicaSetMonitor::Node(HostAndPort("b"), NULL));
        nodes.push_back(ReplicaSetMonitor::Node(HostAndPort("c"), NULL));

        nodes[0].ok = true;
        nodes[1].ok = true;
        nodes[2].ok = true;

        nodes[0].secondary = true;
        nodes[1].ismaster = true;
        nodes[2].secondary = true;

        nodes[0].lastIsMaster = BSON("tags" << BSON("dc" << "nyc" << "p" << "1"));
        nodes[1].lastIsMaster = BSON("tags" << BSON("dc" << "sf"));
        nodes[2].lastIsMaster = BSON("tags" << BSON("dc" << "nyc" << "p" << "2"));

        return nodes;
    }

    class TagSetFixtures {
    public:
        static BSONArray getDefaultSet();
        static BSONArray getP2Tag();
        static BSONArray getSingleNoMatchTag();
        static BSONArray getMultiNoMatchTag();
    };

    BSONArray TagSetFixtures::getDefaultSet() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSONObj());
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getP2Tag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p" << "2"));
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getSingleNoMatchTag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("k" << "x"));
        return arrayBuilder.arr();
    }

    BSONArray TagSetFixtures::getMultiNoMatchTag() {
        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("mongo" << "db"));
        arrayBuilder.append(BSON("by" << "10gen"));
        return arrayBuilder.arr();
    }

    TEST(ReplSetMonitorReadPref, PrimaryOnly) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, PrimaryOnlyPriNotOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3,  &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, PrimaryMissing) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        nodes[1].ismaster = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, PriPrefWithPriOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();

        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, PriPrefWithPriNotOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecOnly) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecOnlyOnlyPriOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[0].ok = false;
        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, SecPref) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecPrefWithNoSecOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[0].ok = false;
        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecPrefWithNoNodeOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[1].addr;

        nodes[0].ok = false;
        nodes[1].ok = false;
        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 1, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, Nearest) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        nodes[0].pingTimeMillis = 1;
        nodes[1].pingTimeMillis = 2;
        nodes[2].pingTimeMillis = 3;

        bool isPrimarySelected = 0;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, NearestNoLocal) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getDefaultSet());
        HostAndPort lastHost = nodes[0].addr;

        nodes[0].pingTimeMillis = 10;
        nodes[1].pingTimeMillis = 20;
        nodes[2].pingTimeMillis = 30;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!host.empty());
    }

    TEST(ReplSetMonitorReadPref, PriOnlyWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getP2Tag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        // Note: PrimaryOnly ignores tag
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, PriPrefPriNotOkWithTags) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getP2Tag());
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, PriPrefPriOkWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, PriPrefPriNotOkWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, SecOnlyWithTags) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getP2Tag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecOnlyWithTagsMatchOnlyPri) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[2].addr;

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("dc" << "sf"));
        TagSet tags(arrayBuilder.arr());

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryOnly, &tags, 3, &lastHost,
             &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, SecPrefWithTags) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getP2Tag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecPrefSecNotOkWithTags) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[1].addr;

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("dc" << "nyc"));
        TagSet tags(arrayBuilder.arr());

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, SecPrefPriOkWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, SecPrefPriNotOkWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, SecPrefPriOkWithSecNotMatchTag) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(ReplSetMonitorReadPref, NearestWithTags) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[1].addr;

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p" << "1"));
        TagSet tags(arrayBuilder.arr());

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, NearestWithTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getSingleNoMatchTag());
        HostAndPort lastHost = nodes[1].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, MultiPriOnlyTag) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[1].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST(ReplSetMonitorReadPref, MultiPriOnlyPriNotOkTag) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[1].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryOnly, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(ReplSetMonitorReadPref, PriPrefPriOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p" << "1"));
        arrayBuilder.append(BSON("p" << "2"));

        TagSet tags(arrayBuilder.arr());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    class MultiTags: public mongo::unittest::Test {
    public:
        vector<ReplicaSetMonitor::Node> getNodes() const {
            return NodeSetFixtures::getThreeMemberWithTags();
        }

        TagSet* getMatchesFirstTagSet() {
            if (matchFirstTags.get() != NULL) {
                return matchFirstTags.get();
            }

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append(BSON("p" << "1"));
            arrayBuilder.append(BSON("p" << "2"));
            matchFirstTags.reset(new TagSet(arrayBuilder.arr()));

            return matchFirstTags.get();
        }

        TagSet* getMatchesSecondTagSet() {
            if (matchSecondTags.get() != NULL) {
                return matchSecondTags.get();
            }

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append(BSON("p" << "3"));
            arrayBuilder.append(BSON("p" << "2"));
            arrayBuilder.append(BSON("p" << "1"));
            matchSecondTags.reset(new TagSet(arrayBuilder.arr()));

            return matchSecondTags.get();
        }

        TagSet* getMatchesLastTagSet() {
            if (matchLastTags.get() != NULL) {
                return matchLastTags.get();
            }

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append(BSON("p" << "12"));
            arrayBuilder.append(BSON("p" << "23"));
            arrayBuilder.append(BSON("p" << "19"));
            arrayBuilder.append(BSON("p" << "34"));
            arrayBuilder.append(BSON("p" << "1"));
            matchLastTags.reset(new TagSet(arrayBuilder.arr()));

            return matchLastTags.get();
        }

        TagSet* getMatchesPriTagSet() {
            if (matchPriTags.get() != NULL) {
                return matchPriTags.get();
            }

            BSONArrayBuilder arrayBuilder;
            arrayBuilder.append(BSON("dc" << "sf"));
            arrayBuilder.append(BSON("p" << "1"));
            matchPriTags.reset(new TagSet(arrayBuilder.arr()));

            return matchPriTags.get();
        }

    private:
        scoped_ptr<TagSet> matchFirstTags;
        scoped_ptr<TagSet> matchSecondTags;
        scoped_ptr<TagSet> matchLastTags;
        scoped_ptr<TagSet> matchPriTags;
    };

    TEST_F(MultiTags, MultiTagsMatchesFirst) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, PriPrefPriNotOkMatchesFirstNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;
        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, PriPrefPriNotOkMatchesSecondTest) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, PriPrefPriNotOkMatchesSecondNotOkTest) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;
        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, PriPrefPriNotOkMatchesLastTest) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, PriPrefPriNotOkMatchesLastNotOkTest) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;
        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(MultiTags, PriPrefPriOkNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();

        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(MultiTags, PriPrefPriNotOkNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_PrimaryPreferred, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST_F(MultiTags, SecOnlyMatchesFirstTest) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMatchesFirstNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMatchesSecond) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMatchesSecondNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMatchesLast) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMatchesLastNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryOnly, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST_F(MultiTags, SecOnlyMultiTagsWithPriMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryOnly, getMatchesPriTagSet(),
             3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecOnlyMultiTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryOnly, &tags, 3, &lastHost,
             &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST_F(MultiTags, SecPrefMatchesFirst) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecPrefMatchesFirstNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, SecPrefMatchesSecond) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, SecPrefMatchesSecondNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesSecondTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecPrefMatchesLast) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, SecPrefMatchesLastNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_SecondaryPreferred, getMatchesLastTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST_F(MultiTags, SecPrefMultiTagsWithPriMatch) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryPreferred, getMatchesPriTagSet(),
             3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(MultiTags, SecPrefMultiTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
             &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
    }

    TEST(MultiTags, SecPrefMultiTagsNoMatchPriNotOk) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        nodes[1].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_SecondaryPreferred, &tags, 3, &lastHost,
             &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST_F(MultiTags, NearestMatchesFirst) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, getMatchesFirstTagSet(),
            3, &lastHost, &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST(MultiTags, NearestMatchesFirstNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = NodeSetFixtures::getThreeMemberWithTags();

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p" << "1"));
        arrayBuilder.append(BSON("dc" << "sf"));

        TagSet tags(arrayBuilder.arr());
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST_F(MultiTags, NearestMatchesSecond) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, getMatchesSecondTagSet(), 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("c", host.host());
        ASSERT_EQUALS("c", lastHost.host());
    }

    TEST_F(MultiTags, NearestMatchesSecondNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[2].addr;

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("z" << "2"));
        arrayBuilder.append(BSON("p" << "2"));
        arrayBuilder.append(BSON("dc" << "sf"));

        TagSet tags(arrayBuilder.arr());

        nodes[2].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
            &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST_F(MultiTags, NearestMatchesLast) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, getMatchesLastTagSet(), 3, &lastHost,
            &isPrimarySelected);

        ASSERT(!isPrimarySelected);
        ASSERT_EQUALS("a", host.host());
        ASSERT_EQUALS("a", lastHost.host());
    }

    TEST_F(MultiTags, NeatestMatchesLastNotOk) {
        vector<ReplicaSetMonitor::Node> nodes = getNodes();
        HostAndPort lastHost = nodes[2].addr;

        nodes[0].ok = false;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
            mongo::ReadPreference_Nearest, getMatchesLastTagSet(), 3, &lastHost,
            &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST_F(MultiTags, NearestMultiTagsWithPriMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_Nearest, getMatchesPriTagSet(), 3, &lastHost,
             &isPrimarySelected);

        ASSERT(isPrimarySelected);
        ASSERT_EQUALS("b", host.host());
        ASSERT_EQUALS("b", lastHost.host());
    }

    TEST(TagSet, CopyConstructor) {
        TagSet* copy;

        {
            BSONArrayBuilder builder;
            builder.append(BSON("dc" << "nyc"));
            builder.append(BSON("priority" << "1"));
            TagSet original(builder.arr());

            original.next();

            copy = new TagSet(original);
        }

        ASSERT_FALSE(copy->isExhausted());
        ASSERT(copy->getCurrentTag().equal(BSON("dc" << "nyc")));
        copy->next();

        ASSERT_FALSE(copy->isExhausted());
        ASSERT(copy->getCurrentTag().equal(BSON("priority" << "1")));
        copy->next();

        ASSERT(copy->isExhausted());

        delete copy;
    }

    TEST(TagSet, NearestMultiTagsNoMatch) {
        vector<ReplicaSetMonitor::Node> nodes =
                NodeSetFixtures::getThreeMemberWithTags();
        TagSet tags(TagSetFixtures::getMultiNoMatchTag());
        HostAndPort lastHost = nodes[2].addr;

        bool isPrimarySelected = false;
        HostAndPort host = ReplicaSetMonitor::selectNode(nodes,
             mongo::ReadPreference_Nearest, &tags, 3, &lastHost,
             &isPrimarySelected);

        ASSERT(host.empty());
    }

    TEST(TagSet, SingleTagSet) {
        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "nyc"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!tags.isExhausted());
        ASSERT(tags.getCurrentTag().equal(BSON("dc" << "nyc")));

        ASSERT(!tags.isExhausted());
        tags.next();

        ASSERT(tags.isExhausted());
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
        // TODO: remove this guard once SERVER-6317 is fixed
        ASSERT_THROWS(tags.getCurrentTag(), mongo::AssertionException);
#endif
    }

    TEST(TagSet, MultiTagSet) {
        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "nyc"));
        builder.append(BSON("dc" << "sf"));
        builder.append(BSON("dc" << "ma"));

        TagSet tags(BSONArray(builder.done()));

        ASSERT(!tags.isExhausted());
        ASSERT(tags.getCurrentTag().equal(BSON("dc" << "nyc")));

        ASSERT(!tags.isExhausted());
        tags.next();
        ASSERT(tags.getCurrentTag().equal(BSON("dc" << "sf")));

        ASSERT(!tags.isExhausted());
        tags.next();
        ASSERT(tags.getCurrentTag().equal(BSON("dc" << "ma")));

        ASSERT(!tags.isExhausted());
        tags.next();

        ASSERT(tags.isExhausted());
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
        // TODO: remove this guard once SERVER-6317 is fixed
        ASSERT_THROWS(tags.getCurrentTag(), mongo::AssertionException);
#endif
    }

    TEST(TagSet, EmptyArrayTags) {
        BSONArray emptyArray;
        TagSet tags(emptyArray);

        ASSERT(tags.isExhausted());
#if !(defined(_DEBUG) || defined(_DURABLEDEFAULTON) || defined(_DURABLEDEFAULTOFF))
        // TODO: remove this guard once SERVER-6317 is fixed
        ASSERT_THROWS(tags.getCurrentTag(), mongo::AssertionException);
#endif
    }

    TEST(TagSet, Reset) {
        BSONArrayBuilder builder;
        builder.append(BSON("dc" << "nyc"));

        TagSet tags(BSONArray(builder.done()));
        tags.next();
        ASSERT(tags.isExhausted());

        tags.reset();

        ASSERT(!tags.isExhausted());
        ASSERT(tags.getCurrentTag().equal(BSON("dc" << "nyc")));
    }

    // TODO: Port these existing tests here: replmonitor_bad_seed.js, repl_monitor_refresh.js

    /**
     * Warning: Tests running this fixture cannot be run in parallel with other tests
     * that uses ConnectionString::setConnectionHook
     */
    class ReplicaSetMonitorTest: public mongo::unittest::Test {
    protected:
        void setUp() {
            _replSet.reset(new MockReplicaSet("test", 3));
            _originalConnectionHook = ConnectionString::getConnectionHook();
            ConnectionString::setConnectionHook(
                    mongo::MockConnRegistry::get()->getConnStrHook());
        }

        void tearDown() {
            ConnectionString::setConnectionHook(_originalConnectionHook);
            ReplicaSetMonitor::cleanup();
            _replSet.reset();
            mongo::ScopedDbConnection::clearPool();
        }

        MockReplicaSet* getReplSet() {
            return _replSet.get();
        }

    private:
        ConnectionString::ConnectionHook* _originalConnectionHook;
        boost::scoped_ptr<MockReplicaSet> _replSet;
    };

    TEST_F(ReplicaSetMonitorTest, SeedWithPriOnlySecDown) {
        // Test to make sure that the monitor doesn't crash when
        // ConnectionString::connect returns NULL
        MockReplicaSet* replSet = getReplSet();
        replSet->kill(replSet->getSecondaries());

        // Create a monitor with primary as the only seed list and the two secondaries
        // down so a NULL connection object will be stored for these secondaries in
        // the _nodes vector.
        const string replSetName(replSet->getSetName());
        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet->getPrimary()));
        ReplicaSetMonitor::createIfNeeded(replSetName, seedList);

        replSet->kill(replSet->getPrimary());

        ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(replSet->getSetName());
        // Trigger calls to Node::getConnWithRefresh
        monitor->check();
    }

    // Stress test case for a node that is previously a primary being removed from the set.
    // This test goes through configurations with different positions for the primary node
    // in the host list returned from the isMaster command. The test here is to make sure
    // that the ReplicaSetMonitor will not crash under these situations.
    TEST(ReplicaSetMonitorTest, PrimaryRemovedFromSetStress) {
        const size_t NODE_COUNT = 5;
        MockReplicaSet replSet("test", NODE_COUNT);
        ConnectionString::ConnectionHook* originalConnHook =
                ConnectionString::getConnectionHook();
        ConnectionString::setConnectionHook(mongo::MockConnRegistry::get()->getConnStrHook());

        const string replSetName(replSet.getSetName());
        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet.getPrimary()));
        ReplicaSetMonitor::createIfNeeded(replSetName, seedList);

        const MockReplicaSet::ReplConfigMap origConfig = replSet.getReplConfig();
        mongo::ReplicaSetMonitorPtr replMonitor = ReplicaSetMonitor::get(replSetName);

        for (size_t idxToRemove = 0; idxToRemove < NODE_COUNT; idxToRemove++) {
            MockReplicaSet::ReplConfigMap newConfig = origConfig;

            replSet.setConfig(origConfig);
            replMonitor->check(); // Make sure the monitor sees the change

            string hostToRemove;
            {
                BSONObjBuilder monitorStateBuilder;
                replMonitor->appendInfo(monitorStateBuilder);
                BSONObj monitorState = monitorStateBuilder.done();

                BSONElement hostsElem = monitorState["hosts"];
                BSONElement addrElem = hostsElem[mongo::str::stream() << idxToRemove]["addr"];
                hostToRemove = addrElem.String();
            }

            replSet.setPrimary(hostToRemove);
            replMonitor->check(); // Make sure the monitor sees the new primary

            newConfig.erase(hostToRemove);
            replSet.setConfig(newConfig);
            replSet.setPrimary(newConfig.begin()->first);
            replMonitor->check(); // Force refresh -> should not crash
        }

        ReplicaSetMonitor::cleanup();
        ConnectionString::setConnectionHook(originalConnHook);
        mongo::ScopedDbConnection::clearPool();
    }

    /**
     * Warning: Tests running this fixture cannot be run in parallel with other tests
     * that use ConnectionString::setConnectionHook.
     */
    class TwoNodeWithTags: public mongo::unittest::Test {
    protected:
        void setUp() {
            _replSet.reset(new MockReplicaSet("test", 2));
            _originalConnectionHook = ConnectionString::getConnectionHook();
            ConnectionString::setConnectionHook(
                    mongo::MockConnRegistry::get()->getConnStrHook());

            mongo::MockReplicaSet::ReplConfigMap config = _replSet->getReplConfig();

            {
                const string host(_replSet->getPrimary());
                map<string, string>& tag = config[host].tags;
                tag.clear();
                tag["dc"] = "ny";
                tag["num"] = "1";
            }

            {
                const string host(_replSet->getSecondaries().front());
                map<string, string>&  tag = config[host].tags;
                tag.clear();
                tag["dc"] = "ny";
                tag["num"] = "2";
            }

            _replSet->setConfig(config);

        }

        void tearDown() {
            ConnectionString::setConnectionHook(_originalConnectionHook);
            ReplicaSetMonitor::cleanup();
            _replSet.reset();
        }

        MockReplicaSet* getReplSet() {
            return _replSet.get();
        }

    private:
        ConnectionString::ConnectionHook* _originalConnectionHook;
        boost::scoped_ptr<MockReplicaSet> _replSet;
    };

    // Tests the case where the connection to secondary went bad and the replica set
    // monitor needs to perform a refresh of it's local view then retry the node selection
    // again after the refresh.
    TEST_F(TwoNodeWithTags, SecDownRetryNoTag) {
        MockReplicaSet* replSet = getReplSet();

        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet->getPrimary()));
        ReplicaSetMonitor::createIfNeeded(replSet->getSetName(), seedList);

        const string secHost(replSet->getSecondaries().front());
        replSet->kill(secHost);

        ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(replSet->getSetName());
        // Make sure monitor sees the dead secondary
        monitor->check();

        replSet->restore(secHost);

        TagSet tags(BSON_ARRAY(BSONObj()));
        bool isPrimary;
        HostAndPort node = monitor->selectAndCheckNode(mongo::ReadPreference_SecondaryOnly,
                                                       &tags, &isPrimary);
        ASSERT_FALSE(isPrimary);
        ASSERT_EQUALS(secHost, node.toString(true));
    }

    // Tests the case where the connection to secondary went bad and the replica set
    // monitor needs to perform a refresh of it's local view then retry the node selection
    // with tags again after the refresh.
    TEST_F(TwoNodeWithTags, SecDownRetryWithTag) {
        MockReplicaSet* replSet = getReplSet();

        vector<HostAndPort> seedList;
        seedList.push_back(HostAndPort(replSet->getPrimary()));
        ReplicaSetMonitor::createIfNeeded(replSet->getSetName(), seedList);

        const string secHost(replSet->getSecondaries().front());
        replSet->kill(secHost);

        ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(replSet->getSetName());
        // Make sure monitor sees the dead secondary
        monitor->check();

        replSet->restore(secHost);

        TagSet tags(BSON_ARRAY(BSON("dc" << "ny")));
        bool isPrimary;
        HostAndPort node = monitor->selectAndCheckNode(mongo::ReadPreference_SecondaryOnly,
                                                       &tags, &isPrimary);
        ASSERT_FALSE(isPrimary);
        ASSERT_EQUALS(secHost, node.toString(true));
    }
}
