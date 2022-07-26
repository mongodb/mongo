/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/sdam/sdam_test_base.h"

#include <boost/algorithm/string.hpp>
#include <boost/optional/optional_io.hpp>
#include <ostream>
#include <set>

#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/system_clock_source.h"

namespace mongo::sdam {
// Disabling these tests since this causes jstest failures when
// running on a host with a mixed case hostname.
// TEST(ServerDescriptionTest, ShouldNormalizeAddress) {
//    ServerDescription a("foo:1234");
//    ServerDescription b("FOo:1234");
//    ASSERT_EQUALS(a.getAddress(), b.getAddress());
//}

TEST(ServerDescriptionEqualityTest, ShouldCompareDefaultValuesAsEqual) {
    auto a = ServerDescription(HostAndPort("foo:1234"));
    auto b = ServerDescription(HostAndPort("foo:1234"));
    ASSERT_EQUALS(a, b);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareDifferentAddressButSameServerTypeAsEqual) {
    // Note: The SDAM specification does not prescribe how to compare server descriptions with
    // different addresses for equality. We choose that two descriptions are considered equal if
    // their addresses are different.
    auto a = *ServerDescriptionBuilder()
                  .withAddress(HostAndPort("foo:1234"))
                  .withType(ServerType::kStandalone)
                  .instance();
    auto b = *ServerDescriptionBuilder()
                  .withAddress(HostAndPort("bar:1234"))
                  .withType(ServerType::kStandalone)
                  .instance();
    ASSERT_EQUALS(a, b);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareServerTypes) {
    auto a = *ServerDescriptionBuilder().withType(ServerType::kStandalone).instance();
    auto b = *ServerDescriptionBuilder().withType(ServerType::kRSSecondary).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMinWireVersion) {
    auto a = *ServerDescriptionBuilder().withMinWireVersion(1).instance();
    auto b = *ServerDescriptionBuilder().withMinWireVersion(2).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMaxWireVersion) {
    auto a = *ServerDescriptionBuilder().withMaxWireVersion(1).instance();
    auto b = *ServerDescriptionBuilder().withMaxWireVersion(2).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMeValues) {
    auto a = *ServerDescriptionBuilder().withMe(HostAndPort("foo")).instance();
    auto b = *ServerDescriptionBuilder().withMe(HostAndPort("bar")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareHosts) {
    auto a = *ServerDescriptionBuilder().withHost(HostAndPort("foo")).instance();
    auto b = *ServerDescriptionBuilder().withHost(HostAndPort("bar")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldComparePassives) {
    auto a = *ServerDescriptionBuilder().withPassive(HostAndPort("foo")).instance();
    auto b = *ServerDescriptionBuilder().withPassive(HostAndPort("bar")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareArbiters) {
    auto a = *ServerDescriptionBuilder().withArbiter(HostAndPort("foo")).instance();
    auto b = *ServerDescriptionBuilder().withArbiter(HostAndPort("bar")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMultipleHostsOrderDoesntMatter) {
    auto a = *ServerDescriptionBuilder()
                  .withHost(HostAndPort("foo"))
                  .withHost(HostAndPort("bar"))
                  .instance();
    auto b = *ServerDescriptionBuilder()
                  .withHost(HostAndPort("bar"))
                  .withHost(HostAndPort("foo"))
                  .instance();
    ASSERT_EQUALS(a, b);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMultiplePassivesOrderDoesntMatter) {
    auto a = *ServerDescriptionBuilder()
                  .withPassive(HostAndPort("foo"))
                  .withPassive(HostAndPort("bar"))
                  .instance();
    auto b = *ServerDescriptionBuilder()
                  .withPassive(HostAndPort("bar"))
                  .withPassive(HostAndPort("foo"))
                  .instance();
    ASSERT_EQUALS(a, b);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareMultipleArbitersOrderDoesntMatter) {
    auto a = *ServerDescriptionBuilder()
                  .withArbiter(HostAndPort("foo"))
                  .withArbiter(HostAndPort("bar"))
                  .instance();
    auto b = *ServerDescriptionBuilder()
                  .withArbiter(HostAndPort("bar"))
                  .withArbiter(HostAndPort("foo"))
                  .instance();
    ASSERT_EQUALS(a, b);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareTags) {
    auto a = *ServerDescriptionBuilder().withTag("foo", "bar").instance();
    auto b = *ServerDescriptionBuilder().withTag("baz", "buz").instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareSetName) {
    auto a = *ServerDescriptionBuilder().withSetName("foo").instance();
    auto b = *ServerDescriptionBuilder().withSetName("bar").instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareSetVersion) {
    auto a = *ServerDescriptionBuilder().withSetVersion(1).instance();
    auto b = *ServerDescriptionBuilder().withSetVersion(2).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareElectionId) {
    auto a = *ServerDescriptionBuilder().withElectionId(OID::max()).instance();
    auto b = *ServerDescriptionBuilder().withElectionId(OID("000000000000000000000000")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldComparePrimary) {
    auto a = *ServerDescriptionBuilder().withPrimary(HostAndPort("foo:1234")).instance();
    auto b = *ServerDescriptionBuilder().withPrimary(HostAndPort("bar:1234")).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareLogicalSessionTimeout) {
    auto a = *ServerDescriptionBuilder().withLogicalSessionTimeoutMinutes(1).instance();
    auto b = *ServerDescriptionBuilder().withLogicalSessionTimeoutMinutes(2).instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

TEST(ServerDescriptionEqualityTest, ShouldCompareTopologyVersion) {
    auto a =
        *ServerDescriptionBuilder().withTopologyVersion(TopologyVersion(OID::max(), 0)).instance();
    auto b = *ServerDescriptionBuilder()
                  .withTopologyVersion(TopologyVersion(OID("000000000000000000000000"), 0))
                  .instance();
    ASSERT_NOT_EQUALS(a, b);
    ASSERT_EQUALS(a, a);
}

class ServerDescriptionTestFixture : public SdamTestFixture {
protected:
    // returns a set containing the elements in the given bson array with lowercase values.
    std::set<std::string> toHostSet(std::vector<BSONElement> bsonArray) {
        return mapSet(bsonArray, [](const BSONElement& e) { return str::toLower(e.String()); });
    }

    std::map<std::string, std::string> toStringMap(BSONObj bsonObj) {
        std::map<std::string, std::string> result;
        const auto keys = bsonObj.getFieldNames<std::set<std::string>>();
        std::transform(keys.begin(),
                       keys.end(),
                       std::inserter(result, result.begin()),
                       [bsonObj](const std::string& key) {
                           return std::pair<const std::string, std::string>(
                               key, bsonObj.getStringField(key));
                       });
        return result;
    }

    static BSONObjBuilder okBuilder() {
        return std::move(BSONObjBuilder().append("ok", 1));
    }

    static inline auto rand = PseudoRandom(SecureRandom().nextInt64());

    static inline const auto clockSource = SystemClockSource::get();

    static inline const auto kBsonOk = okBuilder().obj();
    static inline const auto kBsonMissingOk = BSONObjBuilder().obj();
    static inline const auto kBsonMongos = okBuilder().append("msg", "isdbgrid").obj();
    static inline const auto kBsonRsPrimary =
        okBuilder().append("ismaster", true).append("setName", "foo").obj();
    static inline const auto kBsonRsSecondary =
        okBuilder().append("secondary", true).append("setName", "foo").obj();
    static inline const auto kBsonRsArbiter =
        okBuilder().append("arbiterOnly", true).append("setName", "foo").obj();
    static inline const auto kBsonRsOther =
        okBuilder().append("hidden", true).append("setName", "foo").obj();
    static inline const auto kBsonRsGhost = okBuilder().append("isreplicaset", true).obj();
    static inline const auto kBsonWireVersion =
        okBuilder().append("minWireVersion", 1).append("maxWireVersion", 2).obj();
    static inline const auto kBsonTags =
        okBuilder()
            .append("tags", BSONObjBuilder().append("foo", "bar").append("baz", "buz").obj())
            .obj();
    static inline const mongo::repl::OpTime kOpTime =
        mongo::repl::OpTime(Timestamp(1568848910), 24);
    static inline const Date_t kLastWriteDate =
        dateFromISOString("2019-09-18T23:21:50Z").getValue();
    static inline const auto kBsonLastWrite =
        okBuilder()
            .append("lastWrite",
                    BSONObjBuilder()
                        .appendTimeT("lastWriteDate", kLastWriteDate.toTimeT())
                        .append("opTime", kOpTime.toBSON())
                        .obj())
            .obj();
    static inline const auto kBsonHostNames = okBuilder()
                                                  .append("me", "Me:1234")
                                                  .appendArray("hosts",
                                                               BSON_ARRAY("Foo:1234"
                                                                          << "Bar:1234"))
                                                  .appendArray("arbiters",
                                                               BSON_ARRAY("Baz:1234"
                                                                          << "Buz:1234"))
                                                  .appendArray("passives",
                                                               BSON_ARRAY("Biz:1234"
                                                                          << "Boz:1234"))
                                                  .obj();
    static inline const auto kBsonSetVersionName =
        okBuilder().append("setVersion", 1).append("setName", "bar").obj();
    static inline const auto kBsonElectionId = okBuilder().append("electionId", OID::max()).obj();
    static inline const auto kBsonPrimary = okBuilder().append("primary", "foo:1234").obj();
    static inline const auto kBsonLogicalSessionTimeout =
        okBuilder().append("logicalSessionTimeoutMinutes", 1).obj();
    static inline const auto kTopologyVersion =
        okBuilder().append("topologyVersion", TopologyVersion(OID::max(), 0).toBSON()).obj();
};

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsUnknownForIsMasterError) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kTopologyVersion, "an error occurred");
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kUnknown, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsUnknownIfOkMissing) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonMissingOk, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kUnknown, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsStandalone) {
    // No "msg: isdbgrid", no setName, and no "isreplicaset: true".
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonOk, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kStandalone, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsMongos) {
    // contains "msg: isdbgrid"
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonMongos, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kMongos, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsRSPrimary) {
    // "ismaster: true", "setName" in response
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsPrimary, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kRSPrimary, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsRSSecondary) {
    // "secondary: true", "setName" in response
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsSecondary, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kRSSecondary, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsArbiter) {
    // "arbiterOnly: true", "setName" in response.
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsArbiter, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kRSArbiter, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsOther) {
    // "hidden: true", "setName" in response, or not primary, secondary, nor arbiter
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsOther, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kRSOther, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldParseTypeAsGhost) {
    // "isreplicaset: true" in response.
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsGhost, HelloRTT::min());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(ServerType::kRSGhost, description.getType());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreErrorDescription) {
    auto errorMsg = "an error occurred";
    auto response = HelloOutcome(HostAndPort("foo:1234"), kTopologyVersion, errorMsg);
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(errorMsg, *description.getError());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreRTTWithNoPreviousLatency) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsPrimary, HelloRTT::max());
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(HelloRTT::max(), *description.getRtt());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreRTTNullWhenServerTypeIsUnknown) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonMissingOk, HelloRTT::max());
    auto description = ServerDescription(clockSource, response, boost::none);
    ASSERT_EQUALS(boost::none, description.getRtt());
}

TEST_F(ServerDescriptionTestFixture,
       ShouldStoreConstantRTTWhenChangingFromOneKnownServerTypeToAnother) {
    // Simulate a non-ping monitoring response.
    auto response = HelloOutcome(HostAndPort("foo:1234"), kBsonRsPrimary);
    auto lastServerDescription = ServerDescriptionBuilder()
                                     .withType(ServerType::kRSSecondary)
                                     .withRtt(mongo::Milliseconds(20))
                                     .instance();

    // Check the RTT is unchanged since the HelloOutcome does not contain an RTT.
    auto description = ServerDescription(clockSource, response, lastServerDescription->getRtt());
    ASSERT_EQUALS(20, durationCount<mongo::Milliseconds>(*description.getRtt()));

    auto response2 = HelloOutcome(HostAndPort("foo:1234"), kBsonRsPrimary);
    auto description2 = ServerDescription(clockSource, response2, description.getRtt());
    ASSERT_EQUALS(20, durationCount<mongo::Milliseconds>(*description2.getRtt()));
}

TEST_F(ServerDescriptionTestFixture, ShouldPreserveRTTPrecisionForMicroseconds) {
    const int numIterations = 100;
    const int minRttMicros = 100;

    const auto randMicroseconds = [](int m) { return Microseconds(rand.nextInt64(m) + m); };
    auto lastServerDescription = ServerDescriptionBuilder()
                                     .withType(ServerType::kRSPrimary)
                                     .withRtt(randMicroseconds(minRttMicros))
                                     .instance();

    for (int i = 0; i < numIterations; ++i) {
        auto lastRtt = *lastServerDescription->getRtt();
        auto response =
            HelloOutcome(HostAndPort("foo:1234"), kBsonRsPrimary, randMicroseconds(minRttMicros));
        lastServerDescription = std::make_shared<ServerDescription>(clockSource, response, lastRtt);
    }

    // assert the value does not decay to zero
    ASSERT_GT(durationCount<Microseconds>(*lastServerDescription->getRtt()), minRttMicros);
    ASSERT_LT(durationCount<Microseconds>(*lastServerDescription->getRtt()), 2 * minRttMicros);
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreLastWriteDate) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonLastWrite, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kLastWriteDate, description.getLastWriteDate());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreOpTime) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonLastWrite, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kOpTime, description.getOpTime());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreLastUpdateTime) {
    auto testStart = clockSource->now();
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonRsPrimary, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_GREATER_THAN_OR_EQUALS(description.getLastUpdateTime(), testStart);
}

// Disabling these tests since this causes jstest failures when
// running on a host with a mixed case hostname.
// TEST_F(ServerDescriptionTestFixture, ShouldStoreHostNamesAsLowercase) {
//    auto response = HelloOutcome(HostAndPort("FOO:1234"), kBsonHostNames,
//    duration_cast<HelloRTT>(mongo::Milliseconds(40)));
//    auto description = ServerDescription(clockSource, response);
//
//    ASSERT_EQUALS("foo:1234", description.getAddress());
//
//    ASSERT_EQUALS(boost::to_lower_copy(std::string(kBsonHostNames.getStringField("me"))),
//                  *description.getMe());
//
//    auto expectedHosts = toHostSet(kBsonHostNames.getField("hosts").Array());
//    ASSERT_EQUALS(expectedHosts, description.getHosts());
//
//    auto expectedPassives = toHostSet(kBsonHostNames.getField("passives").Array());
//    ASSERT_EQUALS(expectedPassives, description.getPassives());
//
//    auto expectedArbiters = toHostSet(kBsonHostNames.getField("arbiters").Array());
//    ASSERT_EQUALS(expectedArbiters, description.getArbiters());
//}

TEST_F(ServerDescriptionTestFixture, ShouldStoreMinMaxWireVersion) {
    auto response = HelloOutcome(HostAndPort("foo:1234"),
                                 kBsonWireVersion,
                                 duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kBsonWireVersion["minWireVersion"].Int(), description.getMinWireVersion());
    ASSERT_EQUALS(kBsonWireVersion["maxWireVersion"].Int(), description.getMaxWireVersion());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreTags) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonTags, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(adaptForAssert(toStringMap(kBsonTags["tags"].Obj())),
                  adaptForAssert(description.getTags()));
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreSetVersionAndName) {
    auto response = HelloOutcome(HostAndPort("foo:1234"),
                                 kBsonSetVersionName,
                                 duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kBsonSetVersionName.getIntField("setVersion"),
                  description.getElectionIdSetVersionPair().setVersion);
    ASSERT_EQUALS(std::string(kBsonSetVersionName.getStringField("setName")),
                  description.getSetName());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreElectionId) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonElectionId, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kBsonElectionId.getField("electionId").OID(),
                  description.getElectionIdSetVersionPair().electionId);
}

TEST_F(ServerDescriptionTestFixture, ShouldStorePrimary) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonPrimary, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(std::string(kBsonPrimary.getStringField("primary")),
                  description.getPrimary()->toString());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreLogicalSessionTimeout) {
    auto response = HelloOutcome(HostAndPort("foo:1234"),
                                 kBsonLogicalSessionTimeout,
                                 duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(kBsonLogicalSessionTimeout.getIntField("logicalSessionTimeoutMinutes"),
                  description.getLogicalSessionTimeoutMinutes());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreTopologyVersion) {
    auto response = HelloOutcome(HostAndPort("foo:1234"),
                                 kTopologyVersion,
                                 duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto topologyVersion = TopologyVersion::parse(
        IDLParserContext("TopologyVersion"), kTopologyVersion.getObjectField("topologyVersion"));

    auto description =
        ServerDescription(clockSource, response, boost::none /*lastRtt*/, topologyVersion);
    ASSERT_EQUALS(topologyVersion.getProcessId(), description.getTopologyVersion()->getProcessId());
    ASSERT_EQUALS(topologyVersion.getCounter(), description.getTopologyVersion()->getCounter());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreServerAddressOnError) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kTopologyVersion, "an error occurred");
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(std::string("foo:1234"), description.getAddress().toString());
}

TEST_F(ServerDescriptionTestFixture, ShouldStoreCorrectDefaultValuesOnSuccess) {
    auto response = HelloOutcome(
        HostAndPort("foo:1234"), kBsonOk, duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(boost::none, description.getError());
    ASSERT_EQUALS(boost::none, description.getLastWriteDate());
    ASSERT_EQUALS(0, description.getMinWireVersion());
    ASSERT_EQUALS(0, description.getMaxWireVersion());
    ASSERT_EQUALS(boost::none, description.getMe());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getHosts().size());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getPassives().size());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getTags().size());
    ASSERT_EQUALS(boost::none, description.getSetName());
    ASSERT_EQUALS(boost::none, description.getElectionIdSetVersionPair().setVersion);
    ASSERT_EQUALS(boost::none, description.getElectionIdSetVersionPair().electionId);
    ASSERT_EQUALS(boost::none, description.getPrimary());
    ASSERT_EQUALS(boost::none, description.getLogicalSessionTimeoutMinutes());
    ASSERT(boost::none == description.getTopologyVersion());
}


TEST_F(ServerDescriptionTestFixture, ShouldStoreCorrectDefaultValuesOnFailure) {
    auto response = HelloOutcome(HostAndPort("foo:1234"), kTopologyVersion, "an error occurred");
    auto description = ServerDescription(clockSource, response);
    ASSERT_EQUALS(boost::none, description.getLastWriteDate());
    ASSERT_EQUALS(ServerType::kUnknown, description.getType());
    ASSERT_EQUALS(0, description.getMinWireVersion());
    ASSERT_EQUALS(0, description.getMaxWireVersion());
    ASSERT_EQUALS(boost::none, description.getMe());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getHosts().size());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getPassives().size());
    ASSERT_EQUALS(static_cast<size_t>(0), description.getTags().size());
    ASSERT_EQUALS(boost::none, description.getSetName());
    ASSERT_EQUALS(boost::none, description.getElectionIdSetVersionPair().setVersion);
    ASSERT_EQUALS(boost::none, description.getElectionIdSetVersionPair().electionId);
    ASSERT_EQUALS(boost::none, description.getPrimary());
    ASSERT_EQUALS(boost::none, description.getLogicalSessionTimeoutMinutes());
    ASSERT(boost::none == description.getTopologyVersion());
}
};  // namespace mongo::sdam
