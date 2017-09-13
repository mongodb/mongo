/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(LegacyRequest, RoundTrip) {
    auto databaseName = "barbaz";
    auto commandName = "foobar";

    BSONObjBuilder metadataBob{};
    metadataBob.append("$replData", BSONObj());
    auto metadata = metadataBob.done();

    BSONObjBuilder commandArgsBob{};
    commandArgsBob.append(commandName, "baz");
    auto commandArgs = commandArgsBob.done();

    auto request = OpMsgRequest::fromDBAndBody(databaseName, commandArgs, metadata);
    request.sequences.push_back({"sequence", {BSON("a" << 1), BSON("b" << 2)}});
    auto msg = rpc::legacyRequestFromOpMsgRequest(request);

    auto metadataAndSequece = BSONObjBuilder(metadata)
                                  .append("sequence", BSON_ARRAY(BSON("a" << 1) << BSON("b" << 2)))
                                  .obj();

    auto parsed = rpc::opMsgRequestFromLegacyRequest(msg);
    ASSERT_BSONOBJ_EQ(
        parsed.body,
        OpMsgRequest::fromDBAndBody(databaseName, commandArgs, metadataAndSequece).body);
}

TEST(LegacyRequestBuilder, DownconvertSecondaryReadPreference) {
    auto readPref = BSON("mode"
                         << "secondary");
    auto msg = rpc::legacyRequestFromOpMsgRequest(
        OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1 << "$readPreference" << readPref)));
    auto parsed = QueryMessage(msg);

    ASSERT_EQ(parsed.ns, "admin.$cmd"_sd);
    ASSERT_EQ(parsed.queryOptions, QueryOption_SlaveOk);
    ASSERT_BSONOBJ_EQ(parsed.query,
                      fromjson("{$query: {ping: 1}, $readPreference : {mode: 'secondary'}}"));
}

TEST(CommandRequestBuilder, DownconvertExplicitPrimaryReadPreference) {
    auto readPref = BSON("mode"
                         << "primary");
    auto msg = rpc::legacyRequestFromOpMsgRequest(
        OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1 << "$readPreference" << readPref)));
    auto parsed = QueryMessage(msg);

    ASSERT_EQ(parsed.ns, "admin.$cmd"_sd);
    ASSERT_EQ(parsed.queryOptions, 0);
    ASSERT_BSONOBJ_EQ(parsed.query,
                      fromjson("{$query: {ping: 1}, $readPreference : {mode: 'primary'}}"));
}

TEST(CommandRequestBuilder, DownconvertImplicitPrimaryReadPreference) {
    auto msg =
        rpc::legacyRequestFromOpMsgRequest(OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1)));
    auto parsed = QueryMessage(msg);

    ASSERT_EQ(parsed.ns, "admin.$cmd"_sd);
    ASSERT_EQ(parsed.queryOptions, 0);
    ASSERT_BSONOBJ_EQ(parsed.query, fromjson("{ping: 1}"));
}

}  // namespace
