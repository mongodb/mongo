/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <iterator>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/command_request_builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/message.h"

namespace {

using namespace mongo;

TEST(CommandRequest, ParseAllFields) {
    std::vector<char> opCommandData;

    using std::begin;
    using std::end;

    auto writeString = [&opCommandData](const std::string& str) {
        opCommandData.insert(end(opCommandData), begin(str), end(str));
        opCommandData.push_back('\0');
    };

    auto writeObj = [&opCommandData](const BSONObj& obj) {
        opCommandData.insert(end(opCommandData), obj.objdata(), obj.objdata() + obj.objsize());
    };

    auto database = std::string{"ookokokokok"};
    writeString(database);

    auto commandName = std::string{"baz"};
    writeString(commandName);

    BSONObjBuilder commandArgsBob{};
    commandArgsBob.append("baz", "garply");
    auto commandArgs = commandArgsBob.done();
    writeObj(commandArgs);

    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    writeObj(metadata);

    BSONObjBuilder inputDoc1Bob{};
    inputDoc1Bob.append("meep", "boop").append("meow", "chirp");
    auto inputDoc1 = inputDoc1Bob.done();
    writeObj(inputDoc1);

    BSONObjBuilder inputDoc2Bob{};
    inputDoc1Bob.append("bleep", "bop").append("woof", "squeak");
    auto inputDoc2 = inputDoc2Bob.done();
    writeObj(inputDoc2);

    Message toSend;
    toSend.setData(dbCommand, opCommandData.data(), opCommandData.size());

    rpc::CommandRequest opCmd{&toSend};

    ASSERT_EQUALS(opCmd.getCommandName(), commandName);
    ASSERT_EQUALS(opCmd.getDatabase(), database);
    ASSERT_EQUALS(opCmd.getMetadata(), metadata);
    ASSERT_EQUALS(opCmd.getCommandArgs(), commandArgs);

    auto inputDocRange = opCmd.getInputDocs();
    auto inputDocRangeIter = inputDocRange.begin();

    ASSERT_EQUALS(*inputDocRangeIter, inputDoc1);
    // can't use assert equals since we don't have an op to print the iter.
    ASSERT_FALSE(inputDocRangeIter == inputDocRange.end());
    ++inputDocRangeIter;
    ASSERT_EQUALS(*inputDocRangeIter, inputDoc2);
    ASSERT_FALSE(inputDocRangeIter == inputDocRange.end());
    ++inputDocRangeIter;

    ASSERT_TRUE(inputDocRangeIter == inputDocRange.end());
}

TEST(CommandRequest, InvalidNSThrows) {
    rpc::CommandRequestBuilder crb;
    crb.setDatabase("foo////!!!!<><><>");
    crb.setCommandName("ping");
    crb.setCommandArgs(BSON("ping" << 1));
    crb.setMetadata(BSONObj());
    auto msg = crb.done();
    ASSERT_THROWS_CODE(rpc::CommandRequest{&msg}, AssertionException, ErrorCodes::InvalidNamespace);
}

TEST(CommandRequest, EmptyCommandObjThrows) {
    rpc::CommandRequestBuilder crb;
    crb.setDatabase("someDb");
    crb.setCommandName("ping");
    crb.setCommandArgs(BSONObj());
    crb.setMetadata(BSONObj());
    auto msg = crb.done();
    ASSERT_THROWS_CODE(rpc::CommandRequest{&msg}, UserException, 39950);
}

TEST(CommandRequest, MismatchBetweenCommandNamesThrows) {
    rpc::CommandRequestBuilder crb;
    crb.setDatabase("someDb");
    crb.setCommandName("ping");
    crb.setCommandArgs(BSON("launchMissiles" << 1));
    crb.setMetadata(BSONObj());
    auto msg = crb.done();
    ASSERT_THROWS_CODE(rpc::CommandRequest{&msg}, UserException, 39950);
}

}  // namespace
