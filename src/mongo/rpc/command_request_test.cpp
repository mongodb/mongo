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

    Message toSend;
    toSend.setData(dbCommand, opCommandData.data(), opCommandData.size());

    auto opCmd = rpc::ParsedOpCommand::parse(toSend);

    ASSERT_EQUALS(opCmd.body.firstElementFieldName(), commandName);
    ASSERT_EQUALS(opCmd.database, database);
    ASSERT_BSONOBJ_EQ(opCmd.metadata, metadata);
    ASSERT_BSONOBJ_EQ(opCmd.body, commandArgs);
}

TEST(CommandRequest, EmptyCommandObjThrows) {
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

    auto database = std::string{"someDb"};
    writeString(database);

    auto commandName = std::string{"baz"};
    writeString(commandName);

    auto commandArgs = BSONObj();
    writeObj(commandArgs);

    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    writeObj(metadata);

    Message msg;
    msg.setData(dbCommand, opCommandData.data(), opCommandData.size());

    ASSERT_THROWS_CODE(rpc::ParsedOpCommand::parse(msg), UserException, 39950);
}

TEST(CommandRequest, MismatchBetweenCommandNamesThrows) {
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

    auto database = std::string{"someDb"};
    writeString(database);

    auto commandName = std::string{"fakeName"};
    writeString(commandName);

    auto commandArgs = BSON("realName" << 1);
    writeObj(commandArgs);

    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    writeObj(metadata);

    Message msg;
    msg.setData(dbCommand, opCommandData.data(), opCommandData.size());

    ASSERT_THROWS_CODE(rpc::ParsedOpCommand::parse(msg), UserException, 39950);
}

}  // namespace
