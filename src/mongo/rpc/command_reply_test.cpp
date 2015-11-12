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

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/command_reply.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"

namespace {

using namespace mongo;

using std::begin;
using std::end;

class ReplyTest : public mongo::unittest::Test {
protected:
    std::vector<char> _cmdData{};
    Message _message{};

    virtual void setUp() override {
        _message.reset();
    }

    virtual void tearDown() override {
        _cmdData.clear();
    }

    void writeObj(const BSONObj& obj) {
        _cmdData.insert(end(_cmdData), obj.objdata(), obj.objdata() + obj.objsize());
    }

    void writeObj(const BSONObj& obj, std::size_t length) {
        _cmdData.insert(end(_cmdData), obj.objdata(), obj.objdata() + length);
    }

    const Message* buildMessage() {
        _cmdData.shrink_to_fit();
        _message.setData(dbCommandReply, _cmdData.data(), _cmdData.size());
        return &_message;
    }
};

TEST_F(ReplyTest, ParseAllFields) {
    BSONObjBuilder commandReplyBob{};
    commandReplyBob.append("baz", "garply");
    auto commandReply = commandReplyBob.done();
    writeObj(commandReply);

    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    writeObj(metadata);

    BSONObjBuilder outputDoc1Bob{};
    outputDoc1Bob.append("meep", "boop").append("meow", "chirp");
    auto outputDoc1 = outputDoc1Bob.done();
    writeObj(outputDoc1);

    BSONObjBuilder outputDoc2Bob{};
    outputDoc1Bob.append("bleep", "bop").append("woof", "squeak");
    auto outputDoc2 = outputDoc2Bob.done();
    writeObj(outputDoc2);

    rpc::CommandReply opCmdReply{buildMessage()};

    ASSERT_EQUALS(opCmdReply.getMetadata(), metadata);
    ASSERT_EQUALS(opCmdReply.getCommandReply(), commandReply);

    auto outputDocRange = opCmdReply.getOutputDocs();
    auto outputDocRangeIter = outputDocRange.begin();

    ASSERT_EQUALS(*outputDocRangeIter, outputDoc1);
    // can't use assert equals since we don't have an op to print the iter.
    ASSERT_FALSE(outputDocRangeIter == outputDocRange.end());
    ++outputDocRangeIter;
    ASSERT_EQUALS(*outputDocRangeIter, outputDoc2);
    ASSERT_FALSE(outputDocRangeIter == outputDocRange.end());
    ++outputDocRangeIter;

    ASSERT_TRUE(outputDocRangeIter == outputDocRange.end());
}

TEST_F(ReplyTest, EmptyMessageThrows) {
    ASSERT_THROWS(rpc::CommandReply{buildMessage()}, UserException);
}

TEST_F(ReplyTest, MetadataOnlyThrows) {
    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    writeObj(metadata);

    ASSERT_THROWS(rpc::CommandReply{buildMessage()}, UserException);
}

TEST_F(ReplyTest, MetadataInvalidLengthThrows) {
    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    auto trueSize = metadata.objsize();
    // write a super long length field
    DataView(const_cast<char*>(metadata.objdata())).write<LittleEndian<int32_t>>(100000);
    writeObj(metadata, trueSize);
    // write a valid commandReply
    BSONObjBuilder commandReplyBob{};
    commandReplyBob.append("baz", "garply");
    auto commandReply = commandReplyBob.done();
    writeObj(commandReply);

    ASSERT_THROWS(rpc::CommandReply{buildMessage()}, UserException);
}

TEST_F(ReplyTest, InvalidLengthThrows) {
    BSONObjBuilder metadataBob{};
    metadataBob.append("foo", "bar");
    auto metadata = metadataBob.done();
    // write a valid metadata object
    writeObj(metadata);

    BSONObjBuilder commandReplyBob{};
    commandReplyBob.append("baz", "garply");
    auto commandReply = commandReplyBob.done();
    auto trueSize = commandReply.objsize();
    // write a super long length field
    DataView(const_cast<char*>(commandReply.objdata())).write<LittleEndian<int32_t>>(100000);
    writeObj(commandReply, trueSize);

    ASSERT_THROWS(rpc::CommandReply{buildMessage()}, UserException);
}
}
