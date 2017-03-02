/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/base/status.h"
#include "bongo/db/jsobj.h"
#include "bongo/db/wire_version.h"
#include "bongo/rpc/protocol.h"
#include "bongo/unittest/unittest.h"

namespace {

using bongo::WireVersion;
using namespace bongo::rpc;
using bongo::unittest::assertGet;
using bongo::BSONObj;

// Checks if negotiation of the first to protocol sets results in the 'proto'
const auto assert_negotiated = [](ProtocolSet fst, ProtocolSet snd, Protocol proto) {
    auto negotiated = negotiate(fst, snd);
    ASSERT_TRUE(negotiated.isOK());
    ASSERT_TRUE(negotiated.getValue() == proto);
};

TEST(Protocol, SuccessfulNegotiation) {
    assert_negotiated(supports::kAll, supports::kAll, Protocol::kOpCommandV1);
    assert_negotiated(supports::kAll, supports::kOpCommandOnly, Protocol::kOpCommandV1);
    assert_negotiated(supports::kAll, supports::kOpQueryOnly, Protocol::kOpQuery);
}

// Checks that negotiation fails
const auto assert_not_negotiated = [](ProtocolSet fst, ProtocolSet snd) {
    auto proto = negotiate(fst, snd);
    ASSERT_TRUE(!proto.isOK());
    ASSERT_TRUE(proto.getStatus().code() == bongo::ErrorCodes::RPCProtocolNegotiationFailed);
};

TEST(Protocol, FailedNegotiation) {
    assert_not_negotiated(supports::kOpQueryOnly, supports::kOpCommandOnly);
    assert_not_negotiated(supports::kAll, supports::kNone);
    assert_not_negotiated(supports::kOpQueryOnly, supports::kNone);
    assert_not_negotiated(supports::kOpCommandOnly, supports::kNone);
}

TEST(Protocol, parseProtocolSetFromIsMasterReply) {
    {
        // BongoDB 3.2 (bongod)
        auto bongod32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(bongod32)).protocolSet,
                  supports::kAll);
    }
    {
        // BongoDB 3.2 (bongos)
        auto bongos32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE)
                                  << "msg"
                                  << "isdbgrid");

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(bongos32)).protocolSet,
                  supports::kOpQueryOnly);
    }
    {
        // BongoDB 3.0 (bongod)
        auto bongod30 = BSON(
            "maxWireVersion" << static_cast<int>(WireVersion::RELEASE_2_7_7) << "minWireVersion"
                             << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));
        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(bongod30)).protocolSet,
                  supports::kOpQueryOnly);
    }
    {
        auto bongod24 = BSONObj();
        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(bongod24)).protocolSet,
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
    // Base Test
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);

    // Allowed during upgrade
    // BongoD 3.4 client -> BongoD 3.4 server
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);

    // BongoD 3.4 client -> BongoD 3.2 server
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::FIND_COMMAND);

    // BongoD 3.2 client -> BongoD 3.4 server
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);

    // BongoS 3.4 client -> BongoD 3.4 server
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);

    // BongoS 3.2 client -> BongoD 3.4 server
    VALIDATE_WIRE_VERSION(ASSERT_OK,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::FIND_COMMAND,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);

    // Disallowed
    // BongoS 3.4 -> BongoDB 3.2 server
    VALIDATE_WIRE_VERSION(ASSERT_NOT_OK,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN,
                          WireVersion::RELEASE_2_4_AND_BEFORE,
                          WireVersion::FIND_COMMAND);
}

}  // namespace
