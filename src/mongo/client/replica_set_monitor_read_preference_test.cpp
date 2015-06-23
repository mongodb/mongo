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

#include <set>
#include <vector>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_internal.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::set;
using std::vector;

// Pull nested types to top-level scope
typedef ReplicaSetMonitor::SetState SetState;
typedef SetState::Node Node;

HostAndPort selectNode(const vector<Node>& nodes,
                       ReadPreference pref,
                       const TagSet& tagSet,
                       int latencyThresholdMillis,
                       bool* isPrimarySelected) {
    invariant(!nodes.empty());

    set<HostAndPort> seeds;
    seeds.insert(nodes.front().host);

    SetState set("name", seeds);
    set.nodes = nodes;
    set.latencyThresholdMicros = latencyThresholdMillis * 1000;

    ReadPreferenceSetting criteria(pref, tagSet);
    HostAndPort out = set.getMatchingHost(criteria);
    if (isPrimarySelected && !out.empty()) {
        Node* node = set.findNode(out);
        ASSERT(node);
        *isPrimarySelected = node->isMaster;
    }

    return out;
}

vector<Node> getThreeMemberWithTags() {
    vector<Node> nodes;

    nodes.push_back(Node(HostAndPort("a")));
    nodes.push_back(Node(HostAndPort("b")));
    nodes.push_back(Node(HostAndPort("c")));

    nodes[0].isUp = true;
    nodes[1].isUp = true;
    nodes[2].isUp = true;

    nodes[0].isMaster = false;
    nodes[1].isMaster = true;
    nodes[2].isMaster = false;

    nodes[0].tags = BSON("dc"
                         << "nyc"
                         << "p"
                         << "1");
    nodes[1].tags = BSON("dc"
                         << "sf");
    nodes[2].tags = BSON("dc"
                         << "nyc"
                         << "p"
                         << "2");

    return nodes;
}

BSONArray getDefaultTagSet() {
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSONObj());
    return arrayBuilder.arr();
}

BSONArray getP2TagSet() {
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("p"
                             << "2"));
    return arrayBuilder.arr();
}

BSONArray getSingleNoMatchTag() {
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("k"
                             << "x"));
    return arrayBuilder.arr();
}

BSONArray getMultiNoMatchTag() {
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("mongo"
                             << "db"));
    arrayBuilder.append(BSON("by"
                             << "10gen"));
    return arrayBuilder.arr();
}

TEST(ReplSetMonitorReadPref, PrimaryOnly) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, PrimaryOnlyPriNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, PrimaryMissing) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].isMaster = false;

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, PriPrefWithPriOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, PriPrefWithPriNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT(host.host() == "a" || host.host() == "c");
}

TEST(ReplSetMonitorReadPref, SecOnly) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(ReplSetMonitorReadPref, SecOnlyOnlyPriOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed();
    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 1, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, SecPref) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(ReplSetMonitorReadPref, SecPrefWithNoSecOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed();
    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, SecPrefWithNoNodeOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed();
    nodes[1].markFailed();
    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, NearestAllLocal) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].latencyMicros = 1 * 1000;
    nodes[1].latencyMicros = 2 * 1000;
    nodes[2].latencyMicros = 3 * 1000;

    bool isPrimarySelected = 0;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    // Any host is ok
    ASSERT(!host.empty());
    ASSERT_EQUALS(isPrimarySelected, host.host() == "b");
}

TEST(ReplSetMonitorReadPref, NearestOneLocal) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].latencyMicros = 10 * 1000;
    nodes[1].latencyMicros = 20 * 1000;
    nodes[2].latencyMicros = 30 * 1000;

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT_EQUALS("a", host.host());
    ASSERT(!isPrimarySelected);
}

TEST(ReplSetMonitorReadPref, PriOnlyWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    // Note: PrimaryOnly ignores tag
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, PriPrefPriNotOkWithTags) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST(ReplSetMonitorReadPref, PriPrefPriOkWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, PriPrefPriNotOkWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, SecOnlyWithTags) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST(ReplSetMonitorReadPref, SecOnlyWithTagsMatchOnlyPri) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("dc"
                             << "sf"));
    TagSet tags(arrayBuilder.arr());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, SecPrefWithTags) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST(ReplSetMonitorReadPref, SecPrefSecNotOkWithTags) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("dc"
                             << "nyc"));
    TagSet tags(arrayBuilder.arr());

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(ReplSetMonitorReadPref, SecPrefPriOkWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, SecPrefPriNotOkWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, SecPrefPriOkWithSecNotMatchTag) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, NearestWithTags) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("p"
                             << "1"));
    TagSet tags(arrayBuilder.arr());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(ReplSetMonitorReadPref, NearestWithTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, MultiPriOnlyTag) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(ReplSetMonitorReadPref, MultiPriOnlyPriNotOkTag) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(ReplSetMonitorReadPref, PriPrefPriOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("p"
                             << "1"));
    arrayBuilder.append(BSON("p"
                             << "2"));

    TagSet tags(arrayBuilder.arr());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

class MultiTags : public mongo::unittest::Test {
public:
    const TagSet& getMatchesFirstTagSet() {
        if (matchFirstTags.get() != NULL) {
            return *matchFirstTags;
        }

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p"
                                 << "1"));
        arrayBuilder.append(BSON("p"
                                 << "2"));
        matchFirstTags.reset(new TagSet(arrayBuilder.arr()));

        return *matchFirstTags;
    }

    const TagSet& getMatchesSecondTagSet() {
        if (matchSecondTags.get() != NULL) {
            return *matchSecondTags;
        }

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p"
                                 << "3"));
        arrayBuilder.append(BSON("p"
                                 << "2"));
        arrayBuilder.append(BSON("p"
                                 << "1"));
        matchSecondTags.reset(new TagSet(arrayBuilder.arr()));

        return *matchSecondTags;
    }

    const TagSet& getMatchesLastTagSet() {
        if (matchLastTags.get() != NULL) {
            return *matchLastTags;
        }

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("p"
                                 << "12"));
        arrayBuilder.append(BSON("p"
                                 << "23"));
        arrayBuilder.append(BSON("p"
                                 << "19"));
        arrayBuilder.append(BSON("p"
                                 << "34"));
        arrayBuilder.append(BSON("p"
                                 << "1"));
        matchLastTags.reset(new TagSet(arrayBuilder.arr()));

        return *matchLastTags;
    }

    const TagSet& getMatchesPriTagSet() {
        if (matchPriTags.get() != NULL) {
            return *matchPriTags;
        }

        BSONArrayBuilder arrayBuilder;
        arrayBuilder.append(BSON("dc"
                                 << "sf"));
        arrayBuilder.append(BSON("p"
                                 << "1"));
        matchPriTags.reset(new TagSet(arrayBuilder.arr()));

        return *matchPriTags;
    }

private:
    std::unique_ptr<TagSet> matchFirstTags;
    std::unique_ptr<TagSet> matchSecondTags;
    std::unique_ptr<TagSet> matchLastTags;
    std::unique_ptr<TagSet> matchPriTags;
};

TEST_F(MultiTags, MultiTagsMatchesFirst) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, PriPrefPriNotOkMatchesFirstNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();
    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, PriPrefPriNotOkMatchesSecondTest) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, PriPrefPriNotOkMatchesSecondNotOkTest) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[1].markFailed();
    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, PriPrefPriNotOkMatchesLastTest) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesLastTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, PriPrefPriNotOkMatchesLastNotOkTest) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();
    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesLastTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(MultiTags, PriPrefPriOkNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();

    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(MultiTags, PriPrefPriNotOkNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTags, SecOnlyMatchesFirstTest) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecOnlyMatchesFirstNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, SecOnlyMatchesSecond) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, SecOnlyMatchesSecondNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecOnlyMatchesLast) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::SecondaryOnly, getMatchesLastTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecOnlyMatchesLastNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::SecondaryOnly, getMatchesLastTagSet(), 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTags, SecOnlyMultiTagsWithPriMatch) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::SecondaryOnly, getMatchesPriTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecOnlyMultiTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTags, SecPrefMatchesFirst) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecPrefMatchesFirstNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, SecPrefMatchesSecond) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, SecPrefMatchesSecondNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecPrefMatchesLast) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesLastTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, SecPrefMatchesLastNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesLastTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTags, SecPrefMultiTagsWithPriMatch) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesPriTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(MultiTags, SecPrefMultiTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(MultiTags, SecPrefMultiTagsNoMatchPriNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTags, NearestMatchesFirst) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesFirstTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST(MultiTags, NearestMatchesFirstNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("p"
                             << "1"));
    arrayBuilder.append(BSON("dc"
                             << "sf"));

    TagSet tags(arrayBuilder.arr());

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTags, NearestMatchesSecond) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesSecondTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTags, NearestMatchesSecondNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("z"
                             << "2"));
    arrayBuilder.append(BSON("p"
                             << "2"));
    arrayBuilder.append(BSON("dc"
                             << "sf"));

    TagSet tags(arrayBuilder.arr());

    nodes[2].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTags, NearestMatchesLast) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesLastTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTags, NeatestMatchesLastNotOk) {
    vector<Node> nodes = getThreeMemberWithTags();

    nodes[0].markFailed();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesLastTagSet(), 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTags, NearestMultiTagsWithPriMatch) {
    vector<Node> nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesPriTagSet(), 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST(MultiTags, NearestMultiTagsNoMatch) {
    vector<Node> nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST(TagSet, DefaultConstructorMatchesAll) {
    TagSet tags;
    ASSERT_EQUALS(tags.getTagBSON(), BSON_ARRAY(BSONObj()));
}
}
