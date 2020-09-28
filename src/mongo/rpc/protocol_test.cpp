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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/protocol.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace {

using mongo::WireVersion;
using mongo::WireVersionInfo;
using namespace mongo::rpc;
using mongo::BSONObj;
using mongo::unittest::assertGet;

using std::vector;

// Checks if negotiation of the first to protocol sets results in the 'proto'
const auto assert_negotiated = [](ProtocolSet fst, ProtocolSet snd, Protocol proto) {
    auto negotiated = negotiate(fst, snd);
    ASSERT_TRUE(negotiated.isOK());
    ASSERT_TRUE(negotiated.getValue() == proto);
};

TEST(Protocol, SuccessfulNegotiation) {
    assert_negotiated(supports::kAll, supports::kAll, Protocol::kOpMsg);
    assert_negotiated(supports::kAll, supports::kOpMsgOnly, Protocol::kOpMsg);
    assert_negotiated(supports::kAll, supports::kOpQueryOnly, Protocol::kOpQuery);
}

// Checks that negotiation fails
const auto assert_not_negotiated = [](ProtocolSet fst, ProtocolSet snd) {
    auto proto = negotiate(fst, snd);
    ASSERT_TRUE(!proto.isOK());
    ASSERT_TRUE(proto.getStatus().code() == mongo::ErrorCodes::RPCProtocolNegotiationFailed);
};

TEST(Protocol, FailedNegotiation) {
    assert_not_negotiated(supports::kOpQueryOnly, supports::kOpMsgOnly);
    assert_not_negotiated(supports::kAll, supports::kNone);
    assert_not_negotiated(supports::kOpQueryOnly, supports::kNone);
    assert_not_negotiated(supports::kOpMsgOnly, supports::kNone);
}

/*
 * Tests the following:
 * - Replies from MongoDB 2.4 and older reply with supports::kOpQueryOnly
 * - Replies from versions of MongoDB older than 3.6 returns supports::kOpQueryOnly
 * - Replies from MongoDB 3.6 reply with supports::kAll
 * - Replies from latest, last-continuous, and last-lts versions of MongoDB returns supports::kAll
 */
TEST(Protocol, parseProtocolSetFromIsMasterReply) {
    {
        // latest version of MongoDB (mongod)
        auto latestMongod =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::LATEST_WIRE_VERSION)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(latestMongod)).protocolSet,
                  supports::kAll);
    }
    {
        // last continuous version of MongoDB (mongod)
        auto lastContMongod =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::LAST_CONT_WIRE_VERSION)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(lastContMongod)).protocolSet,
                  supports::kAll);
    }
    {
        // last LTS version of MongoDB (mongod)
        auto lastLtsMongod =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::LAST_LTS_WIRE_VERSION)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(lastLtsMongod)).protocolSet,
                  supports::kAll);
    }
    {
        // MongoDB 3.6
        auto mongod36 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::SUPPORTS_OP_MSG)  //
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod36)).protocolSet,
                  supports::kAll);
    }
    {
        // MongoDB 3.2 (mongod)
        auto mongod32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod32)).protocolSet,
                  supports::kOpQueryOnly);  // This used to also include OP_COMMAND.
    }
    {
        // MongoDB 3.2 (mongos)
        auto mongos32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE) << "msg"
                                  << "isdbgrid");

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongos32)).protocolSet,
                  supports::kOpQueryOnly);
    }
    {
        // MongoDB 2.4 and earlier do not have maxWireVersion/minWireVersion in their 'isMaster'
        // replies.
        auto mongod24 = BSONObj();
        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod24)).protocolSet,
                  supports::kOpQueryOnly);
    }
}

#define VALIDATE_WIRE_VERSION(macro, clientMin, clientMax, serverMin, serverMax)            \
    do {                                                                                    \
        auto msg = BSON("minWireVersion" << static_cast<int>(serverMin) << "maxWireVersion" \
                                         << static_cast<int>(serverMax));                   \
        auto swReply = parseProtocolSetFromIsMasterReply(msg);                              \
        ASSERT_OK(swReply.getStatus());                                                     \
        macro(validateWireVersion({clientMin, clientMax}, swReply.getValue().version));     \
    } while (0);

TEST(Protocol, validateWireVersion) {
    // Min, max FCV version pairs representing valid WireVersion ranges for variable binary
    // versions used to communicate with the MongoD 'latest' binary version.

    // MongoD 'latest' binary
    vector<WireVersionInfo> mongoDLatestBinaryRanges = {
        // upgraded FCV
        {WireVersion::LATEST_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        // downgraded 'last-cont' FCV
        {WireVersion::LAST_CONT_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION},
        // downgraded 'last-lts' FCV
        {WireVersion::LAST_LTS_WIRE_VERSION, WireVersion::LATEST_WIRE_VERSION}};

    // MongoS binary versions
    vector<WireVersionInfo> mongoSBinaryRanges = {
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
TEST(Protocol, validateWireVersionFailsForUpgradedServerNode) {
    // Server is fully upgraded to the latest wire version.
    auto msg = BSON("minWireVersion" << static_cast<int>(WireVersion::LATEST_WIRE_VERSION)
                                     << "maxWireVersion"
                                     << static_cast<int>(WireVersion::LATEST_WIRE_VERSION));
    auto swReply = parseProtocolSetFromIsMasterReply(msg);
    ASSERT_OK(swReply.getStatus());

    // The client (this mongos server) only has the previous wire version.
    ASSERT_EQUALS(mongo::ErrorCodes::IncompatibleWithUpgradedServer,
                  validateWireVersion(
                      {WireVersion::LATEST_WIRE_VERSION - 1, WireVersion::LATEST_WIRE_VERSION - 1},
                      swReply.getValue().version)
                      .code());
}

}  // namespace
