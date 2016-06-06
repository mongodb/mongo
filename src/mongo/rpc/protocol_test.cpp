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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/protocol.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::WireVersion;
using namespace mongo::rpc;
using mongo::unittest::assertGet;
using mongo::BSONObj;

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
    ASSERT_TRUE(proto.getStatus().code() == mongo::ErrorCodes::RPCProtocolNegotiationFailed);
};

TEST(Protocol, FailedNegotiation) {
    assert_not_negotiated(supports::kOpQueryOnly, supports::kOpCommandOnly);
    assert_not_negotiated(supports::kAll, supports::kNone);
    assert_not_negotiated(supports::kOpQueryOnly, supports::kNone);
    assert_not_negotiated(supports::kOpCommandOnly, supports::kNone);
}

TEST(Protocol, parseProtocolSetFromIsMasterReply) {
    {
        // MongoDB 3.2 (mongod)
        auto mongod32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod32)), supports::kAll);
    }
    {
        // MongoDB 3.2 (mongos)
        auto mongos32 =
            BSON("maxWireVersion" << static_cast<int>(WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN)
                                  << "minWireVersion"
                                  << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE)
                                  << "msg"
                                  << "isdbgrid");

        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongos32)), supports::kOpQueryOnly);
    }
    {
        // MongoDB 3.0 (mongod)
        auto mongod30 = BSON(
            "maxWireVersion" << static_cast<int>(WireVersion::RELEASE_2_7_7) << "minWireVersion"
                             << static_cast<int>(WireVersion::RELEASE_2_4_AND_BEFORE));
        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod30)), supports::kOpQueryOnly);
    }
    {
        auto mongod24 = BSONObj();
        ASSERT_EQ(assertGet(parseProtocolSetFromIsMasterReply(mongod24)), supports::kOpQueryOnly);
    }
}

}  // namespace
