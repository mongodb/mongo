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
#include "mongo/rpc/command_reply.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/document_range.h"
#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;

    static void _testMaxCommandReply(rpc::ReplyBuilderInterface& replyBuilder);
 
    TEST(CommandReplyBuilder, RoundTrip) {

        BSONObjBuilder metadataBob{};
        metadataBob.append("foo", "bar");
        auto metadata = metadataBob.done();

        BSONObjBuilder commandReplyBob{};
        commandReplyBob.append("bar", "baz").append("ok", 1.0);
        auto commandReply = commandReplyBob.done();

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

        rpc::CommandReplyBuilder r;

        auto msg = r.setMetadata(metadata)
                    .setCommandReply(commandReply)
                    .addOutputDocs(outputDocRange)
                    .done();

        rpc::CommandReply parsed(msg.get());

        ASSERT_EQUALS(parsed.getMetadata(), metadata);
        ASSERT_EQUALS(parsed.getCommandReply(), commandReply);
        // need ostream overloads for ASSERT_EQUALS
        ASSERT_TRUE(parsed.getOutputDocs() == outputDocRange);
    }

    TEST(CommandReplyBuilder, MaxCommandReply) {
        rpc::CommandReplyBuilder replyBuilder;
        _testMaxCommandReply(replyBuilder);
    }

    TEST(LegacyReplyBuilder, MaxCommandReply) {
        rpc::LegacyReplyBuilder replyBuilder;
        _testMaxCommandReply(replyBuilder);
    }

    // verify current functionality - later will need to change
    TEST(LegacyReplyBuilderSpaceTest, DocSize) {
        rpc::LegacyReplyBuilder replyBuilder;
        replyBuilder.setMetadata(BSONObj()).setCommandReply(BSONObj());

        std::size_t spaceBefore = replyBuilder.availableSpaceForOutputDocs();
        ASSERT_EQUALS(spaceBefore, 0);

        BSONObjBuilder docBuilder{};
        docBuilder.append("foo", "bar");
        auto doc = docBuilder.done();

        replyBuilder.addOutputDoc(doc); //no-op
        std::size_t spaceAfter = replyBuilder.availableSpaceForOutputDocs();
        ASSERT_EQUALS(spaceAfter, 0);
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

        virtual void tearDown() override {
        }

        std::size_t emptyDocSize = 0;
    };

    TEST_F(CommandReplyBuilderSpaceTest, DocSizeEq) {
        rpc::CommandReplyBuilder replyBuilder;
        replyBuilder.setMetadata(BSONObj()).setCommandReply(BSONObj());

        std::size_t spaceBefore = replyBuilder.availableSpaceForOutputDocs();

        BSONObjBuilder docBuilder{};
        docBuilder.append("foo", "bar");
        auto doc = docBuilder.done();
        std::size_t docSize = doc.objsize();

        replyBuilder.addOutputDoc(doc);
        std::size_t spaceAfter = replyBuilder.availableSpaceForOutputDocs();
        ASSERT_EQUALS(spaceBefore - docSize, spaceAfter);
    }

    // multiple calls to addOutputDoc, no metadata
    TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize1) {
        rpc::CommandReplyBuilder replyBuilder;

        replyBuilder.setMetadata(BSONObj()).setCommandReply(BSONObj());

        std::size_t availSpace = replyBuilder.availableSpaceForOutputDocs();

        while (availSpace > 0) {
            std::size_t payloadSz =
                std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
                emptyDocSize;
            BSONObjBuilder docBuilder{};
            std::string payload = std::string(payloadSz, 'y' );
            docBuilder.append("x", payload);
            auto doc = docBuilder.done();
            replyBuilder.addOutputDoc(doc);
            availSpace = replyBuilder.availableSpaceForOutputDocs();
        }
        auto msg = replyBuilder.done();

        ASSERT_EQUALS(msg->size(), mongo::MaxMessageSizeBytes);
    }

    // multiple calls to addOutputDoc, some metadata
    TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize2) {
        rpc::CommandReplyBuilder replyBuilder;

        BSONObjBuilder metadataBuilder{};
        metadataBuilder.append("foo", "bar");
        auto metadata = metadataBuilder.done();

        BSONObjBuilder commandReplyBuilder{};
        commandReplyBuilder.append("oof", "rab");
        auto commandReply = commandReplyBuilder.done();

        replyBuilder.setMetadata(metadata).setCommandReply(commandReply);

        std::size_t availSpace = replyBuilder.availableSpaceForOutputDocs();

        while (availSpace > 0) {
            std::size_t payloadSz =
                std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
                emptyDocSize;
            BSONObjBuilder docBuilder{};
            std::string payload = std::string(payloadSz, 'y' );
            docBuilder.append("x", payload);
            auto doc = docBuilder.done();
            replyBuilder.addOutputDoc(doc);
            availSpace = replyBuilder.availableSpaceForOutputDocs();
        }
        auto msg = replyBuilder.done();

        ASSERT_EQUALS(msg->size(), mongo::MaxMessageSizeBytes);
    }


    // single call to addOutputDocs
    TEST_F(CommandReplyBuilderSpaceTest, MaxDocSize3) {
        rpc::CommandReplyBuilder replyBuilder;

        BSONObjBuilder metadataBuilder{};
        metadataBuilder.append("foo", "bar");
        auto metadata = metadataBuilder.done();

        BSONObjBuilder commandReplyBuilder{};
        commandReplyBuilder.append("oof", "rab");
        auto commandReply = commandReplyBuilder.done();

        replyBuilder.setMetadata(metadata).setCommandReply(commandReply);

        std::size_t availSpace = replyBuilder.availableSpaceForOutputDocs();

        BufBuilder docs;
        while (availSpace > 0) {
            std::size_t payloadSz =
                std::min(availSpace, static_cast<std::size_t>(mongo::BSONObjMaxUserSize)) -
                emptyDocSize;
            BSONObjBuilder docBuilder{};
            std::string payload = std::string(payloadSz, 'y' );
            docBuilder.append("x", payload);
            auto doc = docBuilder.done();
            availSpace -= doc.objsize();
            doc.appendSelfToBufBuilder(docs);
        }
        rpc::DocumentRange docRange{docs.buf(), docs.buf() + docs.len()};
        replyBuilder.addOutputDocs(docRange);

        auto msg = replyBuilder.done();

        ASSERT_EQUALS(msg->size(), mongo::MaxMessageSizeBytes);
    }

   // call to addCommandReply
   void _testMaxCommandReply(rpc::ReplyBuilderInterface& replyBuilder) {
        BSONObjBuilder docBuilder1{};
        docBuilder1.append("x", "");
        auto emptyDoc = docBuilder1.done();
        std::size_t emptyDocSize = emptyDoc.objsize();

        BSONObjBuilder metadataBuilder{};
        metadataBuilder.append("foo", "bar");
        auto metadata = metadataBuilder.done();
        replyBuilder.setMetadata(metadata);

        std::size_t payloadSz = static_cast<std::size_t>(mongo::BSONObjMaxUserSize) -
            emptyDocSize;

        BSONObjBuilder commandReplyBuilder{};
        std::string payload = std::string(payloadSz, 'y' );
        commandReplyBuilder.append("x", payload);
        auto commandReply = commandReplyBuilder.done();
        ASSERT_EQUALS(commandReply.objsize(), mongo::BSONObjMaxUserSize);

        replyBuilder.setCommandReply(commandReply);
    }

}  // namespace
