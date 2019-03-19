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

#include "mongo/client/mongo_uri.h"

namespace mongo {
namespace {

// -- SetState Construction --
using InitialStateTest = ReplicaSetMonitorTest;

TEST_F(InitialStateTest, InitialStateMongoURI) {
    auto uri = MongoURI::parse("mongodb://a,b,c/?replicaSet=name");
    ASSERT_OK(uri.getStatus());
    auto state = makeState(uri.getValue());
    ASSERT_EQUALS(state->name, "name");
    ASSERT(state->seedNodes == basicSeedsSet);
    ASSERT(state->lastSeenMaster.empty());
    ASSERT_EQUALS(state->nodes.size(), basicSeeds.size());
    for (size_t i = 0; i < basicSeeds.size(); i++) {
        auto node = state->findNode(basicSeeds[i]);
        ASSERT(node);
        ASSERT_EQUALS(node->host.toString(), basicSeeds[i].toString());
        ASSERT(!node->isUp);
        ASSERT(!node->isMaster);
        ASSERT(node->tags.isEmpty());
    }
}

// -- Node operations --
class NodeTest : public ReplicaSetMonitorTest {
public:
    bool isCompatible(const Node& node, ReadPreference pref, const TagSet& tagSet) {
        auto connStr = ConnectionString::forReplicaSet(kSetName, {node.host});
        auto set = makeState(MongoURI(connStr));
        set->nodes.push_back(node);

        ReadPreferenceSetting criteria(pref, tagSet);
        return !set->getMatchingHost(criteria).empty();
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
};


TEST_F(NodeTest, SimpleGoodMatch) {
    Node node(((HostAndPort())));
    node.tags = BSON("dc"
                     << "sf");
    ASSERT(node.matches(BSON("dc"
                             << "sf")));
}

TEST_F(NodeTest, SimpleBadMatch) {
    Node node((HostAndPort()));
    node.tags = BSON("dc"
                     << "nyc");
    ASSERT(!node.matches(BSON("dc"
                              << "sf")));
}

TEST_F(NodeTest, ExactMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(SampleIsMasterDoc["tags"].Obj()));
}

TEST_F(NodeTest, EmptyTag) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSONObj()));
}

TEST_F(NodeTest, MemberNoTagMatchesEmptyTag) {
    Node node((HostAndPort()));
    node.tags = NoTags;
    ASSERT(node.matches(BSONObj()));
}

TEST_F(NodeTest, MemberNoTagDoesNotMatch) {
    Node node((HostAndPort()));
    node.tags = NoTags;
    ASSERT(!node.matches(BSON("dc"
                              << "NYC")));
}

TEST_F(NodeTest, IncompleteMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("dc"
                              << "NYC"
                              << "p"
                              << "2"
                              << "hello"
                              << "world")));
}

TEST_F(NodeTest, PartialMatch) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("dc"
                             << "NYC"
                             << "p"
                             << "2")));
}

TEST_F(NodeTest, SingleTagCrit) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("p"
                             << "2")));
}

TEST_F(NodeTest, BadSingleTagCrit) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("dc"
                              << "SF")));
}

TEST_F(NodeTest, NonExistingFieldTag) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("noSQL"
                              << "Mongo")));
}

TEST_F(NodeTest, UnorederedMatching) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(node.matches(BSON("p"
                             << "2"
                             << "dc"
                             << "NYC")));
}

TEST_F(NodeTest, SameValueDiffKey) {
    Node node((HostAndPort()));
    node.tags = SampleTags;
    ASSERT(!node.matches(BSON("datacenter"
                              << "NYC")));
}

TEST_F(NodeTest, PriNodeCompatibleTag) {
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

TEST_F(NodeTest, SecNodeCompatibleTag) {
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

TEST_F(NodeTest, PriNodeNotCompatibleTag) {
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

TEST_F(NodeTest, SecNodeNotCompatibleTag) {
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

TEST_F(NodeTest, PriNodeCompatiblMultiTag) {
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

TEST_F(NodeTest, SecNodeCompatibleMultiTag) {
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

TEST_F(NodeTest, PriNodeNotCompatibleMultiTag) {
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

TEST_F(NodeTest, SecNodeNotCompatibleMultiTag) {
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

// -- IsMasterReply operations --
using IsMasterReplyTest = ReplicaSetMonitorTest;
TEST_F(IsMasterReplyTest, IsMasterBadParse) {
    BSONObj ismaster = BSON("hosts" << BSON_ARRAY("mongo.example:badport"));
    IsMasterReply imr(HostAndPort("mongo.example:27017"), -1, ismaster);
    ASSERT_EQUALS(imr.ok, false);
}

TEST_F(IsMasterReplyTest, IsMasterReplyRSNotInitiated) {
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

TEST_F(IsMasterReplyTest, IsMasterReplyRSPrimary) {
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

TEST_F(IsMasterReplyTest, IsMasterReplyPassiveSecondary) {
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

TEST_F(IsMasterReplyTest, IsMasterReplyHiddenSecondary) {
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

TEST_F(IsMasterReplyTest, IsMasterSecondaryWithTags) {
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

}  // namespace
}  // namespace mongo
