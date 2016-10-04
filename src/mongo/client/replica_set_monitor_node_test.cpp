/**
 *    Copyright (C) 2015 MongoDB Inc.
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
typedef ReplicaSetMonitor::SetState SetState;
typedef SetState::Node Node;
typedef SetState::Nodes Nodes;

bool isCompatible(const Node& node, ReadPreference pref, const TagSet& tagSet) {
    set<HostAndPort> seeds;
    seeds.insert(node.host);
    SetState set("name", seeds);
    set.nodes.push_back(node);

    ReadPreferenceSetting criteria(pref, tagSet);
    return !set.getMatchingHost(criteria).empty();
}

const BSONObj SampleIsMasterDoc = BSON("tags" << BSON("dc"
                                                      << "NYC"
                                                      << "p"
                                                      << "2"
                                                      << "region"
                                                      << "NA"));
const BSONObj SampleTags = SampleIsMasterDoc["tags"].Obj();
const BSONObj NoTags = BSONObj();
const BSONObj NoTagIsMasterDoc = BSON("isMaster" << true);


TEST(ReplSetMonitorNode, SimpleGoodMatch) {
    Node node(((HostAndPort())));
    node.tags = BSON("dc"
                     << "sf");
    ASSERT(node.matches(BSON("dc"
                             << "sf")));
}

TEST(ReplSetMonitorNode, SimpleBadMatch) {
    Node node((HostAndPort()));
    node.tags = BSON("dc"
                     << "nyc");
    ASSERT(!node.matches(BSON("dc"
                              << "sf")));
}

TEST(ReplSetMonitorNode, ExactMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(SampleIsMasterDoc["tags"].Obj()));
}

TEST(ReplSetMonitorNode, EmptyTag) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSONObj()));
}

TEST(ReplSetMonitorNode, MemberNoTagMatchesEmptyTag) {
    Node node((HostAndPort()));
    node.tags = NoTags;
    ASSERT(node.matches(BSONObj()));
}

TEST(ReplSetMonitorNode, MemberNoTagDoesNotMatch) {
    Node node((HostAndPort()));
    node.tags = NoTags;
    ASSERT(!node.matches(BSON("dc"
                              << "NYC")));
}

TEST(ReplSetMonitorNode, IncompleteMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("dc"
                              << "NYC"
                              << "p"
                              << "2"
                              << "hello"
                              << "world")));
}

TEST(ReplSetMonitorNode, PartialMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("dc"
                             << "NYC"
                             << "p"
                             << "2")));
}

TEST(ReplSetMonitorNode, SingleTagCrit) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("p"
                             << "2")));
}

TEST(ReplSetMonitorNode, BadSingleTagCrit) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("dc"
                              << "SF")));
}

TEST(ReplSetMonitorNode, NonExistingFieldTag) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("noSQL"
                              << "Mongo")));
}

TEST(ReplSetMonitorNode, UnorederedMatching) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("p"
                             << "2"
                             << "dc"
                             << "NYC")));
}

TEST(ReplSetMonitorNode, SameValueDiffKey) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("datacenter"
                              << "NYC")));
}

TEST(ReplSetMonitorNode, PriNodeCompatibleTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = true;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "NYC"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, SecNodeCompatibleTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = false;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "NYC"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, PriNodeNotCompatibleTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = true;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "SF"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, SecNodeNotCompatibleTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = false;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "SF"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, PriNodeCompatiblMultiTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = true;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "RP"));
    builder.append(BSON("dc"
                        << "NYC"
                        << "p"
                        << "2"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, SecNodeCompatibleMultiTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = false;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "RP"));
    builder.append(BSON("dc"
                        << "NYC"
                        << "p"
                        << "2"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, PriNodeNotCompatibleMultiTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = true;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "sf"));
    builder.append(BSON("dc"
                        << "NYC"
                        << "P"
                        << "4"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

TEST(ReplSetMonitorNode, SecNodeNotCompatibleMultiTag) {
    Node node(HostAndPort("dummy", 3));
    node.tags = SampleTags;

    node.isUp = true;
    node.isMaster = false;

    BSONArrayBuilder builder;
    builder.append(BSON("dc"
                        << "sf"));
    builder.append(BSON("dc"
                        << "NYC"
                        << "P"
                        << "4"));

    TagSet tags(BSONArray(builder.done()));

    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::PrimaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryPreferred, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::SecondaryOnly, tags));
    ASSERT(!isCompatible(node, mongo::ReadPreference::Nearest, tags));
}

}  // namespace
