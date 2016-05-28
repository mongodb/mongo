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

#include "mongo/bson/util/builder.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/rpc/command_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

template <typename T>
void testRoundTrip(rpc::ReplyBuilderInterface& replyBuilder);

TEST(LegacyReplyBuilder, RoundTrip) {
    rpc::LegacyReplyBuilder r;
    testRoundTrip<rpc::LegacyReply>(r);
}

TEST(CommandReplyBuilder, RoundTrip) {
    rpc::CommandReplyBuilder r;
    testRoundTrip<rpc::CommandReply>(r);
}

BSONObj buildMetadata() {
    BSONObjBuilder metadataTop{};
    BSONObjBuilder metadataGle{};
    metadataGle.append("lastOpTime", Timestamp());
    metadataGle.append("electionId", OID("5592bee00d21e3aa796e185e"));
    metadataTop.append("$gleStats", metadataGle.done());
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
    a.append(BSON("Foo"
                  << "Bar"));
    a.done();

    cursorBuilder.appendIntOrLL("id", 1);
    cursorBuilder.append("ns", "test.$cmd.blah");
    commandReplyBob.append("cursor", cursorBuilder.done());
    return commandReplyBob.obj();
}

TEST(CommandReplyBuilder, MemAccess) {
    BSONObj metadata = buildMetadata();
    BSONObj commandReply = buildCommand();
    rpc::CommandReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(commandReply);
    replyBuilder.setMetadata(metadata);
    auto msg = replyBuilder.done();

    rpc::CommandReply parsed(&msg);

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_EQUALS(parsed.getCommandReply(), commandReply);
}

TEST(LegacyReplyBuilder, MemAccess) {
    BSONObj metadata = buildMetadata();
    BSONObj commandReply = buildEmptyCommand();
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setRawCommandReply(commandReply);
    replyBuilder.setMetadata(metadata);
    auto msg = replyBuilder.done();

    rpc::LegacyReply parsed(&msg);

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_EQUALS(parsed.getCommandReply(), commandReply);
}

template <typename T>
void testRoundTrip(rpc::ReplyBuilderInterface& replyBuilder) {
    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();

    replyBuilder.setCommandReply(commandReply);
    replyBuilder.setMetadata(metadata);

    BSONObjBuilder outputDoc1Bob{};
    outputDoc1Bob.append("z", "t");
    auto outputDoc1 = outputDoc1Bob.done();

    BSONObjBuilder outputDoc2Bob{};
    outputDoc2Bob.append("h", "j");
    auto outputDoc2 = outputDoc2Bob.done();

    BSONObjBuilder outputDoc3Bob{};
    outputDoc3Bob.append("g", "p");
    auto outputDoc3 = outputDoc3Bob.done();

    BufBuilder outputDocs;
    outputDoc1.appendSelfToBufBuilder(outputDocs);
    outputDoc2.appendSelfToBufBuilder(outputDocs);
    outputDoc3.appendSelfToBufBuilder(outputDocs);
    rpc::DocumentRange outputDocRange{outputDocs.buf(), outputDocs.buf() + outputDocs.len()};
    if (replyBuilder.getProtocol() != rpc::Protocol::kOpQuery) {
        replyBuilder.addOutputDocs(outputDocRange);
    }

    auto msg = replyBuilder.done();

    T parsed(&msg);

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    if (replyBuilder.getProtocol() != rpc::Protocol::kOpQuery) {
        ASSERT_TRUE(parsed.getOutputDocs() == outputDocRange);
    }
}

}  // namespace
