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

#include "mongo/db/jsobj.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(CommandRequestBuilder, RoundTrip) {
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
    auto msg = rpc::opCommandRequestFromOpMsgRequest(request);

    auto bodyAndSequence = BSONObjBuilder(commandArgs)
                               .append("sequence", BSON_ARRAY(BSON("a" << 1) << BSON("b" << 2)))
                               .obj();

    auto parsed = mongo::rpc::ParsedOpCommand::parse(msg);

    ASSERT_EQUALS(parsed.database, databaseName);
    ASSERT_EQUALS(StringData(parsed.body.firstElementFieldName()), commandName);
    ASSERT_BSONOBJ_EQ(parsed.metadata, metadata);
    ASSERT_BSONOBJ_EQ(parsed.body, bodyAndSequence);
}

TEST(CommandRequestBuilder, DownconvertSecondaryReadPreferenceToSSM) {
    auto readPref = BSON("mode"
                         << "secondary");
    auto msg = rpc::opCommandRequestFromOpMsgRequest(
        OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1 << "$readPreference" << readPref)));
    auto parsed = mongo::rpc::ParsedOpCommand::parse(msg);

    ASSERT(!parsed.body.hasField("$readPreference"));
    ASSERT(!parsed.body.hasField("$ssm"));
    ASSERT(!parsed.metadata.hasField("$readPreference"));

    ASSERT_BSONOBJ_EQ(parsed.metadata["$ssm"]["$readPreference"].Obj(), readPref);
    ASSERT(parsed.metadata["$ssm"]["$secondaryOk"].trueValue());
}

TEST(CommandRequestBuilder, DownconvertPrimaryReadPreferenceToSSM) {
    auto readPref = BSON("mode"
                         << "primary");
    auto msg = rpc::opCommandRequestFromOpMsgRequest(
        OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1 << "$readPreference" << readPref)));
    auto parsed = mongo::rpc::ParsedOpCommand::parse(msg);

    ASSERT(!parsed.body.hasField("$readPreference"));
    ASSERT(!parsed.body.hasField("$ssm"));
    ASSERT(!parsed.metadata.hasField("$readPreference"));

    ASSERT(!parsed.metadata["$ssm"]["$secondaryOk"].trueValue());
}

}  // namespace
