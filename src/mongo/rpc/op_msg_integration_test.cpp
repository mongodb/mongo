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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

TEST(OpMsg, UnknownRequiredFlagClosesConnection) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto request = OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1)).serialize();
    OpMsg::setFlag(&request, 1u << 15);  // This should be the last required flag to be assigned.

    Message reply;
    ASSERT(!conn->call(request, reply, /*assertOK*/ false));
}

TEST(OpMsg, UnknownOptionalFlagIsIgnored) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto request = OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1)).serialize();
    OpMsg::setFlag(&request, 1u << 31);  // This should be the last optional flag to be assigned.

    Message reply;
    ASSERT(conn->call(request, reply));
    uassertStatusOK(getStatusFromCommandResult(
        conn->parseCommandReplyMessage(conn->getServerAddress(), reply)->getCommandReply()));
}

TEST(OpMsg, FireAndForgetInsertWorks) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    conn->dropCollection("test.collection");

    conn->runFireAndForgetCommand(OpMsgRequest::fromDBAndBody("test", fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        documents: [
            {a: 1}
        ]
    })")));

    ASSERT_EQ(conn->count("test.collection"), 1u);
}

TEST(OpMsg, DocumentSequenceLargeDocumentMultiInsertWorks) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    conn->dropCollection("test.collection");

    OpMsgBuilder msgBuilder;

    OpMsgBuilder::DocSequenceBuilder sequenceBuilder = msgBuilder.beginDocSequence("documents");
    for (size_t docID = 0; docID < 3; docID++) {
        BSONObjBuilder docBuilder = sequenceBuilder.appendBuilder();
        docBuilder.appendNumber("_id", docID);
        std::string data(15000000, 'a');
        docBuilder.append("data", std::move(data));
    }
    sequenceBuilder.done();


    msgBuilder.setBody(fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        $db: "test"
    })"));

    Message request = msgBuilder.finish();
    Message reply;
    ASSERT_TRUE(conn->call(request, reply, false));

    ASSERT_EQ(conn->count("test.collection"), 3u);
    conn->dropCollection("test.collection");
}

TEST(OpMsg, DocumentSequenceMaxWriteBatchWorks) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    conn->dropCollection("test.collection");

    OpMsgBuilder msgBuilder;

    BSONObj body = fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        $db: "test"
    })");

    constexpr StringData kSequenceName = "documents"_sd;
    size_t targetSize = MaxMessageSizeBytes - body.objsize() - 4 - kSequenceName.size();
    size_t documentSize = targetSize / write_ops::kMaxWriteBatchSize;
    OpMsgBuilder::DocSequenceBuilder sequenceBuilder = msgBuilder.beginDocSequence(kSequenceName);
    for (size_t i = 0; i < write_ops::kMaxWriteBatchSize; i++) {
        BSONObjBuilder docBuilder = sequenceBuilder.appendBuilder();
        docBuilder.append("a", std::string(documentSize - 13, 'b'));
    }
    sequenceBuilder.done();

    msgBuilder.setBody(std::move(body));

    Message request = msgBuilder.finish();
    Message reply;
    ASSERT_TRUE(conn->call(request, reply, false));

    ASSERT_EQ(conn->count("test.collection"), write_ops::kMaxWriteBatchSize);
    conn->dropCollection("test.collection");
}

TEST(OpMsg, CloseConnectionOnFireAndForgetNotWritablePrimaryError) {
    const auto connStr = unittest::getFixtureConnectionString();

    // This test only works against a replica set.
    if (connStr.type() != ConnectionString::SET) {
        return;
    }

    bool foundSecondary = false;
    for (auto host : connStr.getServers()) {
        DBClientConnection conn;
        uassertStatusOK(conn.connect(host, "integration_test"));
        bool isMaster;
        ASSERT(conn.isMaster(isMaster));
        if (isMaster)
            continue;
        foundSecondary = true;

        auto request = OpMsgRequest::fromDBAndBody("test", fromjson(R"({
            insert: "collection",
            writeConcern: {w: 0},
            documents: [
                {a: 1}
            ]
        })"))
                           .serialize();

        // Round-trip command fails with NotWritablePrimary error. Note that this failure is in
        // command dispatch which ignores w:0.
        Message reply;
        ASSERT(conn.call(request, reply, /*assertOK*/ true, nullptr));
        ASSERT_EQ(
            getStatusFromCommandResult(
                conn.parseCommandReplyMessage(conn.getServerAddress(), reply)->getCommandReply()),
            ErrorCodes::NotWritablePrimary);

        // Fire-and-forget closes connection when it sees that error. Note that this is using call()
        // rather than say() so that we get an error back when the connection is closed. Normally
        // using call() if kMoreToCome set results in blocking forever.
        OpMsg::setFlag(&request, OpMsg::kMoreToCome);
        // conn.call() calculated the request checksum, but setFlag() makes it invalid. Clear the
        // checksum so the next conn.call() recalculates it.
        OpMsg::removeChecksum(&request);
        ASSERT(!conn.call(request, reply, /*assertOK*/ false, nullptr));

        uassertStatusOK(conn.connect(host, "integration_test"));  // Reconnect.

        // Disable eager checking of primary to simulate a stepdown occurring after the check. This
        // should respect w:0.
        BSONObj output;
        ASSERT(conn.runCommand("admin",
                               fromjson(R"({
                                   configureFailPoint: 'skipCheckingForNotPrimaryInCommandDispatch',
                                   mode: 'alwaysOn'
                               })"),
                               output))
            << output;
        ON_BLOCK_EXIT([&] {
            uassertStatusOK(conn.connect(host, "integration_test-cleanup"));
            ASSERT(conn.runCommand("admin",
                                   fromjson(R"({
                                          configureFailPoint:
                                              'skipCheckingForNotPrimaryInCommandDispatch',
                                          mode: 'off'
                                      })"),
                                   output))
                << output;
        });


        // Round-trip command claims to succeed due to w:0.
        OpMsg::removeChecksum(&request);
        OpMsg::replaceFlags(&request, 0);
        ASSERT(conn.call(request, reply, /*assertOK*/ true, nullptr));
        ASSERT_OK(getStatusFromCommandResult(
            conn.parseCommandReplyMessage(conn.getServerAddress(), reply)->getCommandReply()));

        // Fire-and-forget should still close connection.
        OpMsg::setFlag(&request, OpMsg::kMoreToCome);
        OpMsg::removeChecksum(&request);
        ASSERT(!conn.call(request, reply, /*assertOK*/ false, nullptr));

        break;
    }
    ASSERT(foundSecondary);
}

TEST(OpMsg, DocumentSequenceReturnsWork) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto opMsgRequest = OpMsgRequest::fromDBAndBody("admin", BSON("echo" << 1));
    opMsgRequest.sequences.push_back({"example", {BSON("a" << 1), BSON("b" << 2)}});
    auto request = opMsgRequest.serialize();

    Message reply;
    ASSERT(conn->call(request, reply));

    auto opMsgReply = OpMsg::parse(reply);
    ASSERT_EQ(opMsgReply.sequences.size(), 1u);

    auto sequence = opMsgReply.getSequence("example");
    ASSERT(sequence);
    ASSERT_EQ(sequence->objs.size(), 2u);

    auto checkSequence = [](auto& bson, auto key, auto val) {
        ASSERT(bson.hasField(key));
        ASSERT_EQ(bson[key].Int(), val);
    };
    checkSequence(sequence->objs[0], "a", 1);
    checkSequence(sequence->objs[1], "b", 2);

    ASSERT_BSONOBJ_EQ(opMsgReply.body.getObjectField("echo"),
                      BSON("echo" << 1 << "$db"
                                  << "admin"));
}

constexpr auto kDisableChecksum = "dbClientConnectionDisableChecksum";

void disableClientChecksum() {
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint(kDisableChecksum);
    failPoint->setMode(FailPoint::alwaysOn);
}

void enableClientChecksum() {
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint(kDisableChecksum);
    failPoint->setMode(FailPoint::off);
}

void exhaustTest(bool enableChecksum) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    // Only test exhaust against a single server.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    if (!enableChecksum) {
        disableClientChecksum();
    }

    ON_BLOCK_EXIT([&] { enableClientChecksum(); });

    NamespaceString nss("test", "coll");

    conn->dropCollection(nss.toString());

    // Insert a few documents.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss.toString(), BSON("_id" << i), 0);
    }

    // Issue a find request to open a cursor but return 0 documents.
    auto findCmd = BSON("find" << nss.coll() << "batchSize" << 0 << "sort" << BSON("_id" << 1));
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(nss.db(), findCmd);
    auto request = opMsgRequest.serialize();

    Message reply;
    ASSERT(conn->call(request, reply));
    auto res = OpMsg::parse(reply).body;
    const long long cursorId = res["cursor"]["id"].numberLong();
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    // Reply has checksum if and only if the request did.
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);

    // Construct getMore request with exhaust flag. Set batch size so we will need multiple batches
    // to exhaust the cursor.
    int batchSize = 2;
    GetMoreRequest gmr(nss, cursorId, batchSize, boost::none, boost::none, boost::none);
    opMsgRequest = OpMsgRequest::fromDBAndBody(nss.db(), gmr.toBSON());
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run getMore to initiate the exhaust stream.
    ASSERT(conn->call(request, reply));
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    ASSERT_EQ(res["cursor"]["id"].numberLong(), cursorId);
    std::vector<BSONElement> nextBatch = res["cursor"]["nextBatch"].Array();
    ASSERT_EQ(nextBatch.size(), 2U);
    ASSERT_BSONOBJ_EQ(nextBatch[0].embeddedObject(), BSON("_id" << 0));
    ASSERT_BSONOBJ_EQ(nextBatch[1].embeddedObject(), BSON("_id" << 1));

    // Receive next exhaust batch.
    conn->recv(reply, lastRequestId);
    lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    ASSERT_EQ(res["cursor"]["id"].numberLong(), cursorId);
    nextBatch = res["cursor"]["nextBatch"].Array();
    ASSERT_EQ(nextBatch.size(), 2U);
    ASSERT_BSONOBJ_EQ(nextBatch[0].embeddedObject(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(nextBatch[1].embeddedObject(), BSON("_id" << 3));

    // Receive terminal batch.
    ASSERT(conn->recv(reply, lastRequestId));
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    ASSERT_EQ(res["cursor"]["id"].numberLong(), 0);
    nextBatch = res["cursor"]["nextBatch"].Array();
    ASSERT_EQ(nextBatch.size(), 1U);
    ASSERT_BSONOBJ_EQ(nextBatch[0].embeddedObject(), BSON("_id" << 4));
}

TEST(OpMsg, ServerHandlesExhaustCorrectly) {
    exhaustTest(false);
}

TEST(OpMsg, ServerHandlesExhaustCorrectlyWithChecksum) {
    exhaustTest(true);
}

TEST(OpMsg, ExhaustWithDBClientCursorBehavesCorrectly) {
    // This test simply tries to verify that using the exhaust option with DBClientCursor works
    // correctly. The externally visible behavior should technically be the same as a non-exhaust
    // cursor. The exhaust cursor should ideally provide a performance win over non-exhaust, but we
    // don't measure that here.
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    // Only test exhaust against a single server.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    NamespaceString nss("test", "coll");
    conn->dropCollection(nss.toString());

    const int nDocs = 5;
    unittest::log() << "Inserting " << nDocs << " documents.";
    for (int i = 0; i < nDocs; i++) {
        auto doc = BSON("_id" << i);
        conn->insert(nss.toString(), doc, 0);
    }

    ASSERT_EQ(conn->count(nss.toString()), size_t(nDocs));
    unittest::log() << "Finished document insertion.";

    // Open an exhaust cursor.
    int batchSize = 2;
    auto cursor =
        conn->query(nss, Query().sort("_id", 1), 0, 0, nullptr, QueryOption_Exhaust, batchSize);

    // Verify that the documents are returned properly. Exhaust cursors should still receive results
    // in batches, so we check that these batches correspond to the given specified batch size.
    ASSERT(cursor->more());
    ASSERT_BSONOBJ_EQ(cursor->next(), BSON("_id" << 0));
    ASSERT(cursor->more());
    ASSERT_BSONOBJ_EQ(cursor->next(), BSON("_id" << 1));
    ASSERT_EQ(cursor->objsLeftInBatch(), 0);

    ASSERT(cursor->more());
    ASSERT_BSONOBJ_EQ(cursor->next(), BSON("_id" << 2));
    ASSERT(cursor->more());
    ASSERT_BSONOBJ_EQ(cursor->next(), BSON("_id" << 3));
    ASSERT_EQ(cursor->objsLeftInBatch(), 0);

    ASSERT(cursor->more());
    ASSERT_BSONOBJ_EQ(cursor->next(), BSON("_id" << 4));
    ASSERT_EQ(cursor->objsLeftInBatch(), 0);

    // Should have consumed all documents at this point.
    ASSERT(!cursor->more());
    ASSERT(cursor->isDead());
}

TEST(OpMsg, ExhaustWorksForAggCursor) {
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    // Only test exhaust against a standalone.
    if (conn->isReplicaSetMember() || conn->isMongos()) {
        return;
    }

    NamespaceString nss("test", "coll");

    conn->dropCollection(nss.toString());

    // Insert 5 documents so that a cursor using a batchSize of 2 requires three batches to get all
    // the results.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss.toString(), BSON("_id" << i));
    }

    // Issue an agg request to open a cursor but return 0 documents. Specify a sort in order to
    // guarantee their return order.
    auto aggCmd = BSON("aggregate" << nss.coll() << "cursor" << BSON("batchSize" << 0) << "pipeline"
                                   << BSON_ARRAY(BSON("$sort" << BSON("_id" << 1))));
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(nss.db(), aggCmd);
    auto request = opMsgRequest.serialize();

    Message reply;
    ASSERT(conn->call(request, reply));
    auto res = OpMsg::parse(reply).body;
    const long long cursorId = res["cursor"]["id"].numberLong();
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    // Construct getMore request with exhaust flag. Set batch size so we will need multiple batches
    // to exhaust the cursor.
    int batchSize = 2;
    GetMoreRequest gmr(nss, cursorId, batchSize, boost::none, boost::none, boost::none);
    opMsgRequest = OpMsgRequest::fromDBAndBody(nss.db(), gmr.toBSON());
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    auto assertNextBatch =
        [](const Message& msg, CursorId expectedCursorId, std::vector<BSONObj> expectedBatch) {
            auto cmdReply = OpMsg::parse(msg).body;
            ASSERT_OK(getStatusFromCommandResult(cmdReply));
            ASSERT_EQ(cmdReply["cursor"]["id"].numberLong(), expectedCursorId);
            std::vector<BSONElement> nextBatch = cmdReply["cursor"]["nextBatch"].Array();
            ASSERT_EQ(nextBatch.size(), expectedBatch.size());
            auto it = expectedBatch.begin();
            for (auto&& batchElt : nextBatch) {
                ASSERT(it != expectedBatch.end());
                ASSERT_BSONOBJ_EQ(batchElt.embeddedObject(), *it);
                ++it;
            }
        };

    // Run getMore to initiate the exhaust stream.
    ASSERT(conn->call(request, reply));
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, cursorId, {BSON("_id" << 0), BSON("_id" << 1)});

    // Receive next exhaust batch.
    ASSERT(conn->recv(reply, lastRequestId));
    lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, cursorId, {BSON("_id" << 2), BSON("_id" << 3)});

    // Receive terminal batch.
    ASSERT(conn->recv(reply, lastRequestId));
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, 0, {BSON("_id" << 4)});
}

void checksumTest(bool enableChecksum) {
    // The server replies with a checksum if and only if the request has a checksum.
    std::string errMsg;
    auto conn = std::unique_ptr<DBClientBase>(
        unittest::getFixtureConnectionString().connect("integration_test", errMsg));
    uassert(ErrorCodes::SocketException, errMsg, conn);

    if (!enableChecksum) {
        disableClientChecksum();
    }

    ON_BLOCK_EXIT([&] { enableClientChecksum(); });

    auto opMsgRequest = OpMsgRequest::fromDBAndBody("admin", BSON("ping" << 1));
    auto request = opMsgRequest.serialize();

    Message reply;
    ASSERT(conn->call(request, reply));

    auto opMsgReply = OpMsg::parse(reply);
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);
}

TEST(OpMsg, ServerRepliesWithoutChecksumToRequestWithoutChecksum) {
    checksumTest(true);
}

TEST(OpMsg, ServerRepliesWithChecksumToRequestWithChecksum) {
    checksumTest(true);
}

TEST(OpMsg, ServerHandlesReallyLargeMessagesGracefully) {
    std::string errMsg;
    auto conn = unittest::getFixtureConnectionString().connect("integration_test", errMsg);
    uassert(ErrorCodes::SocketException, errMsg, conn);

    auto buildInfo = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", BSON("buildInfo" << 1)))
                         ->getCommandReply();
    ASSERT_OK(getStatusFromCommandResult(buildInfo));
    const auto maxBSONObjSizeFromServer =
        static_cast<size_t>(buildInfo["maxBsonObjectSize"].Number());
    const std::string bigData(maxBSONObjSizeFromServer * 2, ' ');

    BSONObjBuilder bob;
    bob << "ismaster" << 1 << "ignoredField" << bigData << "$db"
        << "admin";
    OpMsgRequest request;
    request.body = bob.obj<BSONObj::LargeSizeTrait>();
    ASSERT_GT(request.body.objsize(), BSONObjMaxInternalSize);
    auto requestMsg = request.serialize();

    Message replyMsg;
    ASSERT(conn->call(requestMsg, replyMsg));

    auto reply = OpMsg::parse(replyMsg);
    auto replyStatus = getStatusFromCommandResult(reply.body);
    ASSERT_NOT_OK(replyStatus);
    ASSERT_EQ(replyStatus, ErrorCodes::BSONObjectTooLarge);
}
}  // namespace mongo
