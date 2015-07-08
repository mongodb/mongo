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
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/document_range.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/death_test.h"

namespace {

using namespace mongo;

void testMaxCommandReply(rpc::ReplyBuilderInterface& replyBuilder);

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
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);
    auto msg = replyBuilder.done();

    rpc::CommandReply parsed(msg.get());

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_EQUALS(parsed.getCommandReply(), commandReply);
}

TEST(LegacyReplyBuilder, MemAccess) {
    BSONObj metadata = buildMetadata();
    BSONObj commandReply = buildEmptyCommand();
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);
    auto msg = replyBuilder.done();

    rpc::LegacyReply parsed(msg.get());

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_EQUALS(parsed.getCommandReply(), commandReply);
}

DEATH_TEST(LegacyReplyBuilder, FailureAddingDoc, "Invariant failure _allowAddingOutputDocs") {
    BSONObj metadata = buildMetadata();
    BSONObj commandReply = buildCommand();
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);
    replyBuilder.addOutputDoc(BSONObj());
}

DEATH_TEST(LegacyReplyBuilder, FailureAddingDocs, "Invariant failure _allowAddingOutputDocs") {
    BSONObj metadata = buildMetadata();
    BSONObj commandReply = buildCommand();
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);
    rpc::DocumentRange range;
    replyBuilder.addOutputDocs(range);
}

template <typename T>
void testRoundTrip(rpc::ReplyBuilderInterface& replyBuilder) {
    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();

    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

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
    replyBuilder.addOutputDocs(outputDocRange);

    auto msg = replyBuilder.done();

    T parsed(msg.get());

    ASSERT_EQUALS(parsed.getMetadata(), metadata);
    ASSERT_TRUE(parsed.getOutputDocs() == outputDocRange);
}

TEST(CommandReplyBuilder, MaxCommandReply) {
    rpc::CommandReplyBuilder replyBuilder;
    testMaxCommandReply(replyBuilder);
}

TEST(LegacyReplyBuilder, MaxCommandReply) {
    rpc::LegacyReplyBuilder replyBuilder;
    testMaxCommandReply(replyBuilder);
}

TEST(LegacyReplyBuilderSpaceTest, DocSize) {
    rpc::LegacyReplyBuilder replyBuilder;
    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();

    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

    auto sizeBefore = replyBuilder.availableBytes();

    for (int i = 0; i < 100000; ++i) {
        BSONObjBuilder docBuilder;
        docBuilder.append("foo" + std::to_string(i), "bar" + std::to_string(i));
        auto statusAfter = replyBuilder.addOutputDoc(docBuilder.done());
        ASSERT_TRUE(statusAfter.isOK());
    }

    auto sizeAfter = replyBuilder.availableBytes();
    auto msg = replyBuilder.done();

    // construct an empty message to compare the estimated size difference with
    // the actual difference
    rpc::LegacyReplyBuilder replyBuilder0;
    replyBuilder0.setMetadata(metadata);
    replyBuilder0.setCommandReply(commandReply);
    auto msg0 = replyBuilder0.done();

    QueryResult::View qr0 = msg0->singleData().view2ptr();
    auto dataLen0 = static_cast<std::size_t>(qr0.msgdata().dataLen());

    QueryResult::View qr = msg->singleData().view2ptr();
    auto dataLen = static_cast<std::size_t>(qr.msgdata().dataLen());

    // below tests the array space estimates
    // due to the inaccuracy in size estimation algo the actual size is off by up to 6 bytes
    // on the large # of documents
    ASSERT_EQUALS(sizeBefore - sizeAfter, dataLen - dataLen0 + 5);
}

class CommandReplyBuilderSpaceTest : public mongo::unittest::Test {
protected:
    // compute  an empty doc size to use in follow up tests for payload size computation
    virtual void setUp() override {
        BSONObjBuilder docBuilder1{};
        docBuilder1.append("x", "");
        auto emptyDoc = docBuilder1.done();
        emptyDocSize = emptyDoc.objsize();
    }

    virtual void tearDown() override {}

    std::size_t emptyDocSize = 0u;
};

TEST_F(CommandReplyBuilderSpaceTest, DocSizeEq) {
    rpc::CommandReplyBuilder replyBuilder;
    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

    std::size_t spaceBefore = replyBuilder.availableBytes();

    BSONObjBuilder docBuilder{};
    docBuilder.append("foo", "bar");
    auto doc = docBuilder.done();
    std::size_t docSize = doc.objsize();

    replyBuilder.addOutputDoc(doc);
    std::size_t spaceAfter = replyBuilder.availableBytes();
    ASSERT_EQUALS(spaceBefore - docSize, spaceAfter);
}

// multiple calls to addOutputDoc, no metadata
TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize1) {
    rpc::CommandReplyBuilder replyBuilder;

    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

    std::size_t availSpace = replyBuilder.availableBytes();

    while (availSpace > 0u) {
        std::size_t payloadSz =
            std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
            emptyDocSize;
        BSONObjBuilder docBuilder{};
        std::string payload = std::string(payloadSz, 'y');
        docBuilder.append("x", payload);
        auto doc = docBuilder.done();
        replyBuilder.addOutputDoc(doc);
        availSpace = replyBuilder.availableBytes();
    }
    auto msg = replyBuilder.done();
    auto sizeUInt = static_cast<std::size_t>(msg->size());

    ASSERT_EQUALS(sizeUInt, mongo::MaxMessageSizeBytes);
}

// multiple calls to addOutputDoc, some metadata
TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize2) {
    rpc::CommandReplyBuilder replyBuilder;

    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

    std::size_t availSpace = replyBuilder.availableBytes();

    while (availSpace > 0u) {
        std::size_t payloadSz =
            std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
            emptyDocSize;
        BSONObjBuilder docBuilder{};
        std::string payload = std::string(payloadSz, 'y');
        docBuilder.append("x", payload);
        auto doc = docBuilder.done();
        replyBuilder.addOutputDoc(doc);
        availSpace = replyBuilder.availableBytes();
    }
    auto msg = replyBuilder.done();
    auto sizeUInt = static_cast<std::size_t>(msg->size());

    ASSERT_EQUALS(sizeUInt, mongo::MaxMessageSizeBytes);
}


// single call to addOutputDocs
TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize3) {
    rpc::CommandReplyBuilder replyBuilder;

    auto metadata = buildMetadata();
    auto commandReply = buildEmptyCommand();
    replyBuilder.setMetadata(metadata);
    replyBuilder.setCommandReply(commandReply);

    std::size_t availSpace = replyBuilder.availableBytes();

    BufBuilder docs;
    while (availSpace > 0u) {
        std::size_t payloadSz =
            std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
            emptyDocSize;
        BSONObjBuilder docBuilder{};
        std::string payload = std::string(payloadSz, 'y');
        docBuilder.append("x", payload);
        auto doc = docBuilder.done();
        availSpace -= doc.objsize();
        doc.appendSelfToBufBuilder(docs);
    }
    rpc::DocumentRange docRange{docs.buf(), docs.buf() + docs.len()};
    replyBuilder.addOutputDocs(docRange);

    auto msg = replyBuilder.done();

    auto sizeUInt = static_cast<std::size_t>(msg->size());

    ASSERT_EQUALS(sizeUInt, mongo::MaxMessageSizeBytes);
}

// call to addCommandReply
void testMaxCommandReply(rpc::ReplyBuilderInterface& replyBuilder) {
    BSONObjBuilder docBuilder1{};
    docBuilder1.append("x", "");
    auto emptyDoc = docBuilder1.done();
    std::size_t emptyDocSize = emptyDoc.objsize();

    auto metadata = buildMetadata();
    replyBuilder.setMetadata(metadata);

    auto payloadSz = static_cast<std::size_t>(mongo::BSONObjMaxUserSize) - emptyDocSize;

    BSONObjBuilder commandReplyBuilder{};
    std::string payload = std::string(payloadSz, 'y');
    commandReplyBuilder.append("x", payload);
    auto commandReply = commandReplyBuilder.obj();
    ASSERT_EQUALS(commandReply.objsize(), mongo::BSONObjMaxUserSize);
    replyBuilder.setCommandReply(commandReply);
}

}  // namespace
