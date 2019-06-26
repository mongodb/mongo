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

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor_test_fixture.h"

#include <memory>

#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"

namespace mongo {
namespace {

class ReadPrefTest : public ReplicaSetMonitorTest {
public:
    ReadPrefTest() = default;
    virtual ~ReadPrefTest() = default;

    HostAndPort selectNode(const SetState::Nodes& nodes,
                           ReadPreference pref,
                           const TagSet& tagSet,
                           int latencyThresholdMillis,
                           bool* isPrimarySelected) {
        invariant(!nodes.empty());

        auto connStr = ConnectionString::forReplicaSet(kSetName, {nodes.front().host});
        auto set = makeState(MongoURI(connStr));
        set->nodes = nodes;
        set->latencyThresholdMicros = latencyThresholdMillis * 1000;

        ReadPreferenceSetting criteria(pref, tagSet);
        HostAndPort out = set->getMatchingHost(criteria);
        if (isPrimarySelected && !out.empty()) {
            Node* node = set->findNode(out);
            ASSERT(node);
            *isPrimarySelected = node->isMaster;
        }

        return out;
    }

    std::vector<HostAndPort> selectNodes(const SetState::Nodes& nodes,
                                         ReadPreference pref,
                                         const TagSet& tagSet,
                                         int latencyThresholdMillis,
                                         bool* isPrimarySelected) {
        invariant(!nodes.empty());

        auto connStr = ConnectionString::forReplicaSet(kSetName, {nodes.front().host});
        auto set = makeState(MongoURI(connStr));
        set->nodes = nodes;
        set->latencyThresholdMicros = latencyThresholdMillis * 1000;

        ReadPreferenceSetting criteria(pref, tagSet);
        auto out = set->getMatchingHosts(criteria);
        if (isPrimarySelected && !out.empty()) {
            for (auto& host : out) {
                Node* node = set->findNode(host);
                ASSERT(node);

                if (node->isMaster) {
                    *isPrimarySelected = node->isMaster;
                    break;
                }
            }
        }

        return out;
    }

    auto getThreeMemberWithTags() {
        SetState::Nodes nodes;

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
};

TEST_F(ReadPrefTest, PrimaryOnly) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, PrimaryOnlyMulti) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts =
        selectNodes(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS(hosts.size(), 1ull);
    ASSERT_EQUALS("b", hosts[0].host());
}

TEST_F(ReadPrefTest, PrimaryOnlyPriNotOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, PrimaryMissing) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].isMaster = false;

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, PrimaryMissingMulti) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].isMaster = false;

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts =
        selectNodes(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(hosts.empty());
}

TEST_F(ReadPrefTest, PriPrefWithPriOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, PriPrefWithPriNotOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT(host.host() == "a" || host.host() == "c");
}

TEST_F(ReadPrefTest, SecOnly) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(ReadPrefTest, SecOnlyMulti) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts =
        selectNodes(nodes, mongo::ReadPreference::SecondaryOnly, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    std::sort(hosts.begin(), hosts.end());

    ASSERT_EQUALS(hosts.size(), 2ull);
    ASSERT_EQUALS("a", hosts[0].host());
    ASSERT_EQUALS("c", hosts[1].host());
}

TEST_F(ReadPrefTest, SecOnlyOnlyPriOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 1, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, SecPref) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(ReadPrefTest, SecPrefWithNoSecOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, SecPrefWithNoNodeOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getDefaultTagSet());

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 1, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, NearestAllLocal) {
    auto nodes = getThreeMemberWithTags();
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

TEST_F(ReadPrefTest, NearestOneLocal) {
    auto nodes = getThreeMemberWithTags();
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

TEST_F(ReadPrefTest, PriOnlyWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    // Note: PrimaryOnly ignores tag
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, PriPrefPriNotOkWithTags) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(ReadPrefTest, PriPrefPriOkWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, PriPrefPriNotOkWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, SecOnlyWithTags) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(ReadPrefTest, SecOnlyWithTagsMatchOnlyPri) {
    auto nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("dc"
                             << "sf"));
    TagSet tags(arrayBuilder.arr());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, SecPrefWithTags) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getP2TagSet());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(ReadPrefTest, SecPrefSecNotOkWithTags) {
    auto nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("dc"
                             << "nyc"));
    TagSet tags(arrayBuilder.arr());

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(ReadPrefTest, SecPrefPriOkWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, SecPrefPriNotOkWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, SecPrefPriOkWithSecNotMatchTag) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, NearestWithTags) {
    auto nodes = getThreeMemberWithTags();

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

TEST_F(ReadPrefTest, NearestWithTagsNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getSingleNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, MultiPriOnlyTag) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(ReadPrefTest, MultiPriOnlyPriNotOkTag) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(ReadPrefTest, PriPrefPriOk) {
    auto nodes = getThreeMemberWithTags();

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

class MultiTagsTest : public ReadPrefTest {
public:
    MultiTagsTest() = default;
    virtual ~MultiTagsTest() = default;

    const TagSet& getMatchesFirstTagSet() {
        if (matchFirstTags.get() != nullptr) {
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
        if (matchSecondTags.get() != nullptr) {
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

    const TagSet& getMatchesOnlyFirstTagSet() {
        if (matchOnlyFirstTag.get() != nullptr) {
            return *matchOnlyFirstTag;
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
        matchOnlyFirstTag.reset(new TagSet(arrayBuilder.arr()));

        return *matchOnlyFirstTag;
    }

    const TagSet& getMatchesPriTagSet() {
        if (matchPriTags.get() != nullptr) {
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

    const stdx::unordered_set<std::string> vectorToSet(std::vector<HostAndPort> hosts) {
        stdx::unordered_set<std::string> matchSet;
        std::transform(hosts.begin(),
                       hosts.end(),
                       std::inserter(matchSet, matchSet.begin()),
                       [](const auto& node) { return node.host(); });
        return matchSet;
    }

private:
    std::unique_ptr<TagSet> matchFirstTags;
    std::unique_ptr<TagSet> matchSecondTags;
    std::unique_ptr<TagSet> matchOnlyFirstTag;
    std::unique_ptr<TagSet> matchPriTags;
};

TEST_F(MultiTagsTest, MultiTagsTestMatchesSecondaries) {
    auto nodes = getThreeMemberWithTags();

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::PrimaryPreferred,
                                                 getMatchesFirstTagSet(),
                                                 3,
                                                 &isPrimarySelected);
    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, PriPrefPriNotOkMatchesFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTagsTest, PriPrefPriNotOkMatchesSecondTest) {
    auto nodes = getThreeMemberWithTags();

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::PrimaryPreferred,
                                                 getMatchesSecondTagSet(),
                                                 3,
                                                 &isPrimarySelected);
    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, PriPrefPriNotOkMatchesSecondNotOkTest) {
    auto nodes = getThreeMemberWithTags();

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, PriPrefPriNotOkMatchesOnlySecondTest) {
    auto nodes = getThreeMemberWithTags();

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, PriPrefPriNotOkMatchesOnlySecondNotOkTest) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});
    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::PrimaryPreferred,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, PriPrefPriOkNoMatch) {
    auto nodes = this->getThreeMemberWithTags();

    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTagsTest, PriPrefPriNotOkNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::PrimaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, SecOnlyMatchesFirstTest) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::SecondaryOnly,
                                                 getMatchesFirstTagSet(),
                                                 3,
                                                 &isPrimarySelected);
    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, SecOnlyMatchesFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTagsTest, SecOnlyMatchesSecond) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::SecondaryOnly,
                                                 getMatchesSecondTagSet(),
                                                 3,
                                                 &isPrimarySelected);
    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, SecOnlyMatchesSecondNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecOnlyMatchesOnlyFirst) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecOnlyMatchesOnlyFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryOnly,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, SecOnlyMultiTagsTestWithPriMatch) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::SecondaryOnly, getMatchesPriTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecOnlyMultiTagsTestNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryOnly, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, SecPrefMatchesFirst) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::SecondaryPreferred,
                                                 getMatchesFirstTagSet(),
                                                 3,
                                                 &isPrimarySelected);

    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, SecPrefMatchesFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("c", host.host());
}

TEST_F(MultiTagsTest, SecPrefMatchesSecond) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(nodes,
                                                 mongo::ReadPreference::SecondaryPreferred,
                                                 getMatchesSecondTagSet(),
                                                 3,
                                                 &isPrimarySelected);

    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, SecPrefMatchesSecondNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesSecondTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecPrefMatchesOnlyFirst) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecPrefMatchesOnlyFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesOnlyFirstTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTagsTest, SecPrefMultiTagsTestWithPriMatch) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(nodes,
                                  mongo::ReadPreference::SecondaryPreferred,
                                  getMatchesPriTagSet(),
                                  3,
                                  &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, SecPrefMultiTagsTestNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTagsTest, SecPrefMultiTagsTestNoMatchPriNotOk) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    nodes[1].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::SecondaryPreferred, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, NearestMatchesFirst) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(
        nodes, mongo::ReadPreference::Nearest, getMatchesFirstTagSet(), 3, &isPrimarySelected);

    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, NearestMatchesFirstNotOk) {
    auto nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("p"
                             << "1"));
    arrayBuilder.append(BSON("dc"
                             << "sf"));

    TagSet tags(arrayBuilder.arr());

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTagsTest, NearestMatchesSecond) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(
        nodes, mongo::ReadPreference::Nearest, getMatchesSecondTagSet(), 3, &isPrimarySelected);

    ASSERT_EQUALS(hosts.size(), 2ull);

    ASSERT(!isPrimarySelected);
    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "c"}));
}

TEST_F(MultiTagsTest, NearestMatchesSecondNotOk) {
    auto nodes = getThreeMemberWithTags();

    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("z"
                             << "2"));
    arrayBuilder.append(BSON("p"
                             << "2"));
    arrayBuilder.append(BSON("dc"
                             << "sf"));

    TagSet tags(arrayBuilder.arr());

    nodes[2].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(isPrimarySelected);
    ASSERT_EQUALS("b", host.host());
}

TEST_F(MultiTagsTest, NearestMatchesOnlyFirst) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesOnlyFirstTagSet(), 3, &isPrimarySelected);

    ASSERT(!isPrimarySelected);
    ASSERT_EQUALS("a", host.host());
}

TEST_F(MultiTagsTest, NeatestMatchesLastNotOk) {
    auto nodes = getThreeMemberWithTags();

    nodes[0].markFailed({ErrorCodes::InternalError, "Test error"});

    bool isPrimarySelected = false;
    HostAndPort host = selectNode(
        nodes, mongo::ReadPreference::Nearest, getMatchesOnlyFirstTagSet(), 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, NearestMultiTagsTestWithPriMatch) {
    auto nodes = getThreeMemberWithTags();

    bool isPrimarySelected = false;
    std::vector<HostAndPort> hosts = selectNodes(
        nodes, mongo::ReadPreference::Nearest, getMatchesPriTagSet(), 3, &isPrimarySelected);

    ASSERT_EQUALS(hosts.size(), 2ull);
    ASSERT(isPrimarySelected);

    ASSERT(vectorToSet(hosts) == stdx::unordered_set<std::string>({"a", "b"}));
}

TEST_F(MultiTagsTest, NearestMultiTagsTestNoMatch) {
    auto nodes = getThreeMemberWithTags();
    TagSet tags(getMultiNoMatchTag());

    bool isPrimarySelected = false;
    HostAndPort host =
        selectNode(nodes, mongo::ReadPreference::Nearest, tags, 3, &isPrimarySelected);

    ASSERT(host.empty());
}

TEST_F(MultiTagsTest, DefaultConstructorMatchesAll) {
    TagSet tags;
    ASSERT_BSONOBJ_EQ(tags.getTagBSON(), BSON_ARRAY(BSONObj()));
}

}  // namespace
}  // namespace mongo
