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

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace {

using namespace mongo;

template <typename T>
void testRoundTrip(rpc::ReplyBuilderInterface& replyBuilder, bool unifiedBodyAndMetadata);

template <typename T>
void testErrors(rpc::ReplyBuilderInterface& replyBuilder);

TEST(LegacyReplyBuilder, RoundTrip) {
    rpc::LegacyReplyBuilder r;
    testRoundTrip<rpc::LegacyReply>(r, true);
}

TEST(OpMsgReplyBuilder, RoundTrip) {
    rpc::OpMsgReplyBuilder r;
    testRoundTrip<rpc::OpMsgReply>(r, true);
}

template <typename T>
void testErrors(rpc::ReplyBuilderInterface& replyBuilder);

TEST(LegacyReplyBuilder, Errors) {
    rpc::LegacyReplyBuilder r;
    testErrors<rpc::LegacyReply>(r);
}

TEST(OpMsgReplyBuilder, Errors) {
    rpc::OpMsgReplyBuilder r;
    testErrors<rpc::OpMsgReply>(r);
}

BSONObj buildMetadata() {
    BSONObjBuilder metadataTop;
    {
        BSONObjBuilder metadataGle(metadataTop.subobjStart("$gleStats"));
        metadataGle.append("lastOpTime", Timestamp());
        metadataGle.append("electionId", OID("5592bee00d21e3aa796e185e"));
    }

    // For now we don't need a real $clusterTime and just ensure that it just round trips whatever
    // is there. If that ever changes, we will need to construct a real $clusterTime here.
    metadataTop.append("$clusterTime", BSON("bogus" << true));
    return metadataTop.obj();
}

BSONObj buildEmptyCommand() {
    const char text[] = "{ ok: 1.0, cursor: { firstBatch: [] } }";
    mongo::BSONObj obj = mongo::fromjson(text);
    return obj;
}

BSONObj buildCommand() {
    BSONObjBuilder commandReplyBob{};
    commandReplyBob.append("ok", 1.0);
    BSONObjBuilder cursorBuilder;
    BSONArrayBuilder a(cursorBuilder.subarrayStart("firstBatch"));
    a.append(BSON("Foo" << "Bar"));
    a.done();

    cursorBuilder.appendNumber("id", 1);
    cursorBuilder.append("ns", "test.$cmd.blah");
    commandReplyBob.append("cursor", cursorBuilder.done());
    return commandReplyBob.obj();
}

BSONObj buildErrReply(const Status status, const BSONObj& extraInfo = {}) {
    BSONObjBuilder bob;
    bob.appendElements(extraInfo);
    bob.append("ok", 0.0);
    bob.append("errmsg", status.reason());
    bob.append("code", status.code());
    bob.append("codeName", ErrorCodes::errorString(status.code()));
    return bob.obj();
}

TEST(LegacyReplyBuilder, CommandError) {
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    BSONObj metadata = buildMetadata();
    BSONObjBuilder extra;
    extra.append("a", "b");
    extra.append("c", "d");
    const BSONObj extraObj = extra.obj();
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(status, extraObj);
    replyBuilder.getBodyBuilder().appendElements(metadata);
    auto msg = replyBuilder.done();

    rpc::LegacyReply parsed(&msg);

    const auto body = ([&] {
        BSONObjBuilder unifiedBuilder(buildErrReply(status, extraObj));
        unifiedBuilder.appendElements(metadata);
        return unifiedBuilder.obj();
    }());

    ASSERT_BSONOBJ_EQ(parsed.getCommandReply(), body);
}

TEST(OpMsgReplyBuilder, CommandError) {
    const Status status(ErrorCodes::InvalidLength, "Response payload too long");
    BSONObj metadata = buildMetadata();
    BSONObjBuilder extra;
    extra.append("a", "b");
    extra.append("c", "d");
    const BSONObj extraObj = extra.obj();
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(status, extraObj);
    replyBuilder.getBodyBuilder().appendElements(metadata);
    auto msg = replyBuilder.done();
    msg.header().setId(124);
    msg.header().setResponseToMsgId(123);
    OpMsg::appendChecksum(&msg);

    rpc::OpMsgReply parsed(&msg);

    const auto body = ([&] {
        BSONObjBuilder unifiedBuilder(buildErrReply(status, extraObj));
        unifiedBuilder.appendElements(metadata);
        return unifiedBuilder.obj();
    }());

    ASSERT_BSONOBJ_EQ(parsed.getCommandReply(), body);
}

TEST(OpMsgReplyBuilder, MessageOverBSONSizeLimit) {
    rpc::OpMsgReplyBuilder r;
    std::string bigStr(1024 * 1024 * 16, 'a');

    {
        // 'builder' is an unowned BSONObjBuilder and thus does none of its own size checking,
        // allowing us to grow the OpMsgReplyBuilder past kOpMsgReplyBSONBufferMaxSize (32MB +
        // 64KB).
        auto builder = r.getBodyBuilder();
        for (auto i = 0; i < 3; i++) {
            builder.append("field" + std::to_string(i), bigStr);
        }
    }

    ASSERT_THROWS_CODE(r.done(), DBException, ErrorCodes::BSONObjectTooLarge);
}

template <typename T>
void testRoundTrip(rpc::ReplyBuilderInterface& replyBuilder, bool unifiedBodyAndMetadata) {
    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();

    replyBuilder.setCommandReply(commandReply);
    replyBuilder.getBodyBuilder().appendElements(metadata);

    auto msg = replyBuilder.done();
    msg.header().setId(124);
    msg.header().setResponseToMsgId(123);
    OpMsg::appendChecksum(&msg);
    T parsed(&msg);

    if (unifiedBodyAndMetadata) {
        const auto body = ([&] {
            BSONObjBuilder unifiedBuilder(std::move(commandReply));
            unifiedBuilder.appendElements(metadata);
            return unifiedBuilder.obj();
        }());

        ASSERT_BSONOBJ_EQ(parsed.getCommandReply(), body);
    } else {
        ASSERT_BSONOBJ_EQ(parsed.getCommandReply(), commandReply);
    }
}

template <typename T>
void testErrors(rpc::ReplyBuilderInterface& replyBuilder) {
    ErrorExtraInfoExample::EnableParserForTest whenInScope;

    const auto status = Status(ErrorExtraInfoExample(123), "Why does this keep failing!");

    replyBuilder.setCommandReply(status);
    replyBuilder.getBodyBuilder().appendElements(buildMetadata());

    auto msg = replyBuilder.done();
    msg.header().setId(124);
    msg.header().setResponseToMsgId(123);
    OpMsg::appendChecksum(&msg);

    T parsed(&msg);
    const Status result = getStatusFromCommandResult(parsed.getCommandReply());
    ASSERT_EQ(result, status.code());
    ASSERT_EQ(result.reason(), status.reason());
    ASSERT(result.extraInfo());
    ASSERT(result.extraInfo<ErrorExtraInfoExample>());
    ASSERT_EQ(result.extraInfo<ErrorExtraInfoExample>()->data, 123);
}

}  // namespace
