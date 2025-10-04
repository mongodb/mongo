/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/wire_version.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

TEST(WireVersionTest, ParseWireVersionFromHelloReply) {
    std::vector<std::pair<WireVersion, WireVersion>> minMaxWireVersions{
        {WireVersion::RELEASE_2_4_AND_BEFORE, WireVersion::LATEST_WIRE_VERSION},
        {WireVersion::SUPPORTS_OP_MSG, WireVersion::LATEST_WIRE_VERSION},
        {WireVersion::LATEST_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        {WireVersion::RELEASE_2_4_AND_BEFORE, WireVersion::LAST_CONT_WIRE_VERSION},
        {WireVersion::RELEASE_2_4_AND_BEFORE, WireVersion::LAST_LTS_WIRE_VERSION},
        {WireVersion::RELEASE_2_4_AND_BEFORE, WireVersion::SUPPORTS_OP_MSG},
        {WireVersion::RELEASE_2_4_AND_BEFORE, WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN}};

    for (auto&& wireVersions : minMaxWireVersions) {
        auto helloCmdReply =
            BSON("maxWireVersion" << wireVersions.second << "minWireVersion" << wireVersions.first);
        auto parsedWireVersions =
            unittest::assertGet(wire_version::parseWireVersionFromHelloReply(helloCmdReply));
        ASSERT_EQ(parsedWireVersions.minWireVersion, wireVersions.first);
        ASSERT_EQ(parsedWireVersions.maxWireVersion, wireVersions.second);
    }
}

TEST(WireVersionTest, ParseWireVersionFromHelloReply24AndEarlier) {
    // MongoDB 2.4 and earlier do not have maxWireVersion/minWireVersion in their 'isMaster'
    // replies. The absence of a wire version is interpreted as the min and max wire versions both
    // being 0.
    BSONObj mongod24{};
    auto parsedWireVersions =
        unittest::assertGet(wire_version::parseWireVersionFromHelloReply(mongod24));
    ASSERT_EQ(parsedWireVersions.minWireVersion, WireVersion::RELEASE_2_4_AND_BEFORE);
    ASSERT_EQ(parsedWireVersions.maxWireVersion, WireVersion::RELEASE_2_4_AND_BEFORE);
}

TEST(WireVersionTest, ParseWireVersionFromHelloReplyErrorOnBadWireVersions) {
    std::vector<std::pair<long long, long long>> badWireVersions{
        {WireVersion::RELEASE_2_4_AND_BEFORE, -1},
        {-1, WireVersion::LATEST_WIRE_VERSION},
        {WireVersion::RELEASE_2_4_AND_BEFORE,
         static_cast<long long>(std::numeric_limits<int>::max()) + 1},
        {static_cast<long long>(std::numeric_limits<int>::max()) + 1,
         WireVersion::LATEST_WIRE_VERSION}};

    for (auto&& wireVersions : badWireVersions) {
        auto helloCmdReply =
            BSON("maxWireVersion" << wireVersions.second << "minWireVersion" << wireVersions.first);
        auto parsedWireVersions = wire_version::parseWireVersionFromHelloReply(helloCmdReply);
        ASSERT_EQ(parsedWireVersions.getStatus(), ErrorCodes::IncompatibleServerVersion);
    }
}

#define VALIDATE_WIRE_VERSION(macro, clientMin, clientMax, serverMin, serverMax)              \
    do {                                                                                      \
        auto msg = BSON("minWireVersion" << static_cast<int>(serverMin) << "maxWireVersion"   \
                                         << static_cast<int>(serverMax));                     \
        auto swReply = wire_version::parseWireVersionFromHelloReply(msg);                     \
        ASSERT_OK(swReply.getStatus());                                                       \
        macro(wire_version::validateWireVersion({clientMin, clientMax}, swReply.getValue())); \
    } while (0);

TEST(WireVersionTest, ValidateWireVersion) {
    // Min, max FCV version pairs representing valid WireVersion ranges for variable binary
    // versions used to communicate with the MongoD 'latest' binary version.

    // MongoD 'latest' binary
    std::vector<WireVersionInfo> mongoDLatestBinaryRanges = {
        // upgraded FCV
        {WireVersion::LATEST_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        // downgraded 'last-cont' FCV
        {WireVersion::LAST_CONT_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        // downgraded 'last-lts' FCV
        {WireVersion::LAST_LTS_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION}};

    // MongoS binary versions
    std::vector<WireVersionInfo> mongoSBinaryRanges = {
        // 'latest' binary
        {WireVersion::LATEST_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        // 'last-cont' binary
        {WireVersion::LAST_CONT_WIRE_VERSION, WireVersion::LAST_CONT_WIRE_VERSION},
        // 'last-lts' binary
        {WireVersion::LAST_LTS_WIRE_VERSION, WireVersion::LAST_LTS_WIRE_VERSION}};

    /*
     * Test communication between:
     * MongoD 'latest' binary version <-> MongoD 'latest' binary version
     * 'latest' should always be able to communicate to 'latest' regardless of FCV.
     */

    for (const auto& clientRange : mongoDLatestBinaryRanges) {
        for (const auto& serverRange : mongoDLatestBinaryRanges) {
            // We don't need to test when FCV != 'latest' && client FCV != server FCV because we
            // don't expect users to be in this state when following our recommended
            // upgrade/downgrade procedure.
            if (clientRange.minWireVersion < WireVersion::LATEST_WIRE_VERSION &&
                serverRange.minWireVersion < WireVersion::LATEST_WIRE_VERSION &&
                clientRange.minWireVersion != serverRange.minWireVersion) {
                continue;
            }
            VALIDATE_WIRE_VERSION(ASSERT_OK,
                                  clientRange.minWireVersion,
                                  clientRange.maxWireVersion,
                                  serverRange.minWireVersion,
                                  serverRange.maxWireVersion);
        }
    }

    /*
     * Test communication between:
     * MongoD 'latest' binary version <-> 'last-cont' binary version with 'last-cont' FCV
     */

    // 'latest' binary with 'latest' FCV  -> 'last-cont' binary with 'last-cont' FCV
    VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION);

    // 'latest' binary with 'last-cont' FCV  -> 'last-cont' binary with 'last-cont' FCV
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION);

    // 'last-cont' binary with 'last-cont' FCV -> 'latest' binary with 'latest' FCV
    VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION);

    // 'last-cont' binary with 'last-cont' FCV -> 'latest' binary with 'last-cont' FCV
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION);

    /*
     * Test communication between:
     * MongoD 'latest' binary version <-> 'last-lts' binary version with 'last-lts' FCV
     */

    // 'latest' binary with 'latest' FCV  -> 'last-lts' binary with 'last-lts' FCV
    VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LAST_LTS_WIRE_VERSION);

    // 'latest' binary with 'last-cont' FCV  -> 'last-lts' binary with 'last-lts' FCV
    if (WireVersion::LAST_CONT_WIRE_VERSION != WireVersion::LAST_LTS_WIRE_VERSION) {
        VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                              WireVersion::LAST_CONT_WIRE_VERSION,
                              WireVersion::LATEST_WIRE_VERSION,
                              WireVersion::LAST_LTS_WIRE_VERSION,
                              WireVersion::LAST_LTS_WIRE_VERSION);
    }

    // 'latest' binary with 'last-lts' FCV  -> 'last-lts' binary with 'last-lts' FCV
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION,
                          WireVersion::LAST_CONT_WIRE_VERSION);

    // 'last-lts' binary with 'last-lts' FCV -> 'latest' binary with 'latest' FCV
    VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION);

    // 'last-lts' binary with 'last-lts' FCV -> 'latest' binary with 'last-lts' FCV
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LAST_LTS_WIRE_VERSION,
                          WireVersion::LATEST_WIRE_VERSION);

    /*
     * Test communication between:
     * variable MongoS binary version -> MongoD 'latest' binary version
     * Note that it is disallowed for MongoS to communicate with a lower binary server.
     */

    for (const auto& mongoSRange : mongoSBinaryRanges) {
        for (const auto& mongoDLatestRange : mongoDLatestBinaryRanges) {
            // MongoS 'latest' binary can communicate with all FCV versions.
            // MongoS 'last-cont' binary can only communicate with MongoD downgraded 'last-cont' and
            // 'last-lts' FCV.
            // MongoS 'last-lts' binary can only communicate with MongoD downgraded 'last-lts' FCV.
            if (mongoSRange.minWireVersion == WireVersion::LATEST_WIRE_VERSION ||
                (mongoSRange.minWireVersion == WireVersion::LAST_CONT_WIRE_VERSION &&
                 mongoDLatestRange.minWireVersion == WireVersion::LAST_CONT_WIRE_VERSION) ||
                (mongoSRange.minWireVersion == WireVersion::LAST_LTS_WIRE_VERSION &&
                 mongoDLatestRange.minWireVersion == WireVersion::LAST_LTS_WIRE_VERSION) ||
                (mongoSRange.minWireVersion == WireVersion::LAST_CONT_WIRE_VERSION &&
                 mongoDLatestRange.minWireVersion == WireVersion::LAST_LTS_WIRE_VERSION)) {
                VALIDATE_WIRE_VERSION(ASSERT_OK,
                                      mongoSRange.minWireVersion,
                                      mongoSRange.maxWireVersion,
                                      mongoDLatestRange.minWireVersion,
                                      mongoDLatestRange.maxWireVersion);
            } else {
                VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                                      mongoSRange.minWireVersion,
                                      mongoSRange.maxWireVersion,
                                      mongoDLatestRange.minWireVersion,
                                      mongoDLatestRange.maxWireVersion);
            }
        }
    }
}

// A mongos is unable to communicate with a fully upgraded cluster with a higher wire version.
TEST(WireVersionTest, ValidateWireVersionFailsForUpgradedServerNode) {
    // Server is fully upgraded to the latest wire version.
    auto msg = BSON("minWireVersion" << static_cast<int>(WireVersion::LATEST_WIRE_VERSION)
                                     << "maxWireVersion"
                                     << static_cast<int>(WireVersion::LATEST_WIRE_VERSION));
    auto swReply = wire_version::parseWireVersionFromHelloReply(msg);
    ASSERT_OK(swReply.getStatus());

    // The client (this mongos server) only has the previous wire version.
    ASSERT_EQUALS(mongo::ErrorCodes::IncompatibleWithUpgradedServer,
                  wire_version::validateWireVersion(
                      {WireVersion::LATEST_WIRE_VERSION - 1, WireVersion::LATEST_WIRE_VERSION - 1},
                      swReply.getValue()));
}

TEST(WireSpec, AllGetterReturnExpectedValues) {
    auto serviceCtx = ServiceContext::make();

    WireSpec::Specification testSpec;
    testSpec.isInternalClient = true;
    testSpec.incomingExternalClient = {WireVersion::RELEASE_2_4_AND_BEFORE,
                                       WireVersion::LATEST_WIRE_VERSION};
    testSpec.incomingInternalClient = {WireVersion::SUPPORTS_OP_MSG,
                                       WireVersion::LATEST_WIRE_VERSION};
    testSpec.outgoing = {WireVersion::LATEST_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION};

    WireSpec::getWireSpec(serviceCtx.get()).initialize(std::move(testSpec));

    auto specPtr = WireSpec::getWireSpec(serviceCtx.get()).get();
    ASSERT(specPtr);

    ASSERT_EQ(specPtr->isInternalClient,
              WireSpec::getWireSpec(serviceCtx.get()).isInternalClient());

    auto externalFromGetter = WireSpec::getWireSpec(serviceCtx.get()).getIncomingExternalClient();
    ASSERT_EQ(specPtr->incomingExternalClient.minWireVersion, externalFromGetter.minWireVersion);
    ASSERT_EQ(specPtr->incomingExternalClient.maxWireVersion, externalFromGetter.maxWireVersion);

    auto internalFromGetter = WireSpec::getWireSpec(serviceCtx.get()).getIncomingInternalClient();
    ASSERT_EQ(specPtr->incomingInternalClient.minWireVersion, internalFromGetter.minWireVersion);
    ASSERT_EQ(specPtr->incomingInternalClient.maxWireVersion, internalFromGetter.maxWireVersion);

    auto outgoingFromGetter = WireSpec::getWireSpec(serviceCtx.get()).getOutgoing();
    ASSERT_EQ(specPtr->outgoing.minWireVersion, outgoingFromGetter.minWireVersion);
    ASSERT_EQ(specPtr->outgoing.maxWireVersion, outgoingFromGetter.maxWireVersion);
}


}  // namespace
}  // namespace mongo
