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
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

template <typename F>
bool waitForCondition(F&& f) {
    // Wait up to 10 seconds.
    bool val = false;
    int i = 0;
    while (!val && i < 100) {
        val = f();
        if (val) {
            return true;
        }
        sleepmillis(100);
        i++;
    }
    return false;
}

std::unique_ptr<DBClientBase> getIntegrationTestConnection() {
    auto swConn = unittest::getFixtureConnectionString().connect("integration_test");
    uassertStatusOK(swConn.getStatus());
    return std::move(swConn.getValue());
}


// Returns the connection name by filtering on the appName of a $currentOp command. If no result is
// found, return an empty string.
std::string getThreadNameByAppName(DBClientBase* conn, StringData appName) {
    auto curOpCmd =
        BSON("aggregate" << 1 << "cursor" << BSONObj() << "pipeline"
                         << BSON_ARRAY(BSON("$currentOp" << BSON("localOps" << true))
                                       << BSON("$match" << BSON("appName" << appName))));
    const auto curOpReply = conn->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, curOpCmd));
    const auto cursorResponse = CursorResponse::parseFromBSON(curOpReply->getCommandReply());
    ASSERT_OK(cursorResponse.getStatus());
    const auto batch = cursorResponse.getValue().getBatch();
    return batch.empty() ? "" : std::string{batch[0].getStringField("desc")};
}

TEST(OpMsg, UnknownRequiredFlagClosesConnection) {
    auto conn = getIntegrationTestConnection();

    auto request = OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                               DatabaseName::kAdmin,
                                               BSON("ping" << 1))
                       .serialize();
    OpMsg::setFlag(&request, 1u << 15);  // This should be the last required flag to be assigned.

    Message reply;
    ASSERT_THROWS_CODE(conn->call(request), DBException, ErrorCodes::HostUnreachable);
}

TEST(OpMsg, UnknownOptionalFlagIsIgnored) {
    auto conn = getIntegrationTestConnection();

    auto request = OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                               DatabaseName::kAdmin,
                                               BSON("ping" << 1))
                       .serialize();
    OpMsg::setFlag(&request, 1u << 31);  // This should be the last optional flag to be assigned.

    Message reply = conn->call(request);
    uassertStatusOK(getStatusFromCommandResult(
        conn->parseCommandReplyMessage(conn->getServerAddress(), reply)->getCommandReply()));
}

TEST(OpMsg, FireAndForgetInsertWorks) {
    auto conn = getIntegrationTestConnection();

    conn->dropCollection(NamespaceString::createNamespaceString_forTest("test.collection"));

    conn->runFireAndForgetCommand(
        OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                    DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                                    fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        documents: [
            {a: 1}
        ]
    })")));

    ASSERT_EQ(conn->count(NamespaceString::createNamespaceString_forTest("test.collection")), 1u);
}

TEST(OpMsg, DocumentSequenceLargeDocumentMultiInsertWorks) {
    auto conn = getIntegrationTestConnection();

    conn->dropCollection(NamespaceString::createNamespaceString_forTest("test.collection"));

    OpMsgBuilder msgBuilder;

    OpMsgBuilder::DocSequenceBuilder sequenceBuilder = msgBuilder.beginDocSequence("documents");
    for (size_t docID = 0; docID < 3; docID++) {
        BSONObjBuilder docBuilder = sequenceBuilder.appendBuilder();
        docBuilder.appendNumber("_id", static_cast<long long>(docID));
        std::string data(15000000, 'a');
        docBuilder.append("data", std::move(data));
    }
    sequenceBuilder.done();


    msgBuilder.setBody(fromjson(R"({
        insert: "collection",
        writeConcern: {w: 0},
        $db: "test"
    })"));

    Message request = msgBuilder.finishWithoutSizeChecking();
    Message reply = conn->call(request);

    ASSERT_EQ(conn->count(NamespaceString::createNamespaceString_forTest("test.collection")), 3u);
    conn->dropCollection(NamespaceString::createNamespaceString_forTest("test.collection"));
}

TEST(OpMsg, DocumentSequenceMaxWriteBatchWorks) {
    auto conn = getIntegrationTestConnection();

    conn->dropCollection(NamespaceString::createNamespaceString_forTest("test.collection"));

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

    Message request = msgBuilder.finishWithoutSizeChecking();
    Message reply = conn->call(request);

    ASSERT_EQ(conn->count(NamespaceString::createNamespaceString_forTest("test.collection")),
              write_ops::kMaxWriteBatchSize);
    conn->dropCollection(NamespaceString::createNamespaceString_forTest("test.collection"));
}

TEST(OpMsg, CloseConnectionOnFireAndForgetNotWritablePrimaryError) {
    const auto connStr = unittest::getFixtureConnectionString();

    // This test only works against a replica set.
    if (connStr.type() != ConnectionString::ConnectionType::kReplicaSet) {
        return;
    }

    bool foundSecondary = false;
    for (auto host : connStr.getServers()) {
        DBClientConnection conn;
        conn.connect(host, "integration_test", boost::none);
        bool isPrimary;
        ASSERT(conn.isPrimary(isPrimary));
        if (isPrimary)
            continue;
        foundSecondary = true;

        auto request = OpMsgRequestBuilder::create(
                           auth::ValidatedTenancyScope::kNotRequired,
                           DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                           fromjson(R"({
            insert: "collection",
            writeConcern: {w: 0},
            documents: [
                {a: 1}
            ]
        })"))
                           .serialize();

        // Round-trip command fails with NotWritablePrimary error. Note that this failure is in
        // command dispatch which ignores w:0.
        Message reply = conn.call(request);
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
        ASSERT_THROWS(conn.call(request), ExceptionFor<ErrorCategory::NetworkError>);

        conn.connect(host, "integration_test", boost::none);  // Reconnect.

        // Disable eager checking of primary to simulate a stepdown occurring after the check. This
        // should respect w:0.
        BSONObj output;
        ASSERT(conn.runCommand(DatabaseName::kAdmin,
                               fromjson(R"({
                                   configureFailPoint: 'skipCheckingForNotPrimaryInCommandDispatch',
                                   mode: 'alwaysOn'
                               })"),
                               output))
            << output;
        ON_BLOCK_EXIT([&] {
            conn.connect(host, "integration_test-cleanup", boost::none);
            ASSERT(conn.runCommand(DatabaseName::kAdmin,
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
        reply = conn.call(request);
        ASSERT_OK(getStatusFromCommandResult(
            conn.parseCommandReplyMessage(conn.getServerAddress(), reply)->getCommandReply()));

        // Fire-and-forget should still close connection.
        OpMsg::setFlag(&request, OpMsg::kMoreToCome);
        OpMsg::removeChecksum(&request);
        ASSERT_THROWS(conn.call(request), ExceptionFor<ErrorCategory::NetworkError>);

        break;
    }
    ASSERT(foundSecondary);
}

TEST(OpMsg, DocumentSequenceReturnsWork) {
    auto conn = getIntegrationTestConnection();

    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, BSON("echo" << 1));
    opMsgRequest.sequences.push_back({"example", {BSON("a" << 1), BSON("b" << 2)}});
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);

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

constexpr auto kDisableChecksum = "dbClientSessionDisableChecksum";

void disableClientChecksum() {
    auto failPoint = globalFailPointRegistry().find(kDisableChecksum);
    failPoint->setMode(FailPoint::alwaysOn);
}

void enableClientChecksum() {
    auto failPoint = globalFailPointRegistry().find(kDisableChecksum);
    failPoint->setMode(FailPoint::off);
}

void exhaustGetMoreTest(bool enableChecksum) {
    auto conn = getIntegrationTestConnection();

    // Only test exhaust against a standalone and mongos.
    if (conn->isReplicaSetMember()) {
        return;
    }

    if (!enableChecksum) {
        disableClientChecksum();
    }

    ON_BLOCK_EXIT([&] { enableClientChecksum(); });

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

    conn->dropCollection(nss);

    // Insert a few documents.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss, BSON("_id" << i));
    }

    // Issue a find request to open a cursor but return 0 documents. Specify a sort in order to
    // guarantee their return order.
    auto findCmd = BSON("find" << nss.coll() << "batchSize" << 0 << "sort" << BSON("_id" << 1));
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), findCmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    const long long cursorId = res["cursor"]["id"].numberLong();
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    // Reply has checksum if and only if the request did.
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);

    // Construct getMore request with exhaust flag. Set batch size so we will need multiple batches
    // to exhaust the cursor.
    int batchSize = 2;
    GetMoreCommandRequest getMoreRequest(cursorId, std::string{nss.coll()});
    getMoreRequest.setBatchSize(batchSize);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), getMoreRequest.toBSON());
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run getMore to initiate the exhaust stream.
    reply = conn->call(request);
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
    reply = conn->recv(lastRequestId);
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
    reply = conn->recv(lastRequestId);
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    ASSERT_EQ(OpMsg::isFlagSet(reply, OpMsg::kChecksumPresent), enableChecksum);
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    ASSERT_EQ(res["cursor"]["id"].numberLong(), 0);
    nextBatch = res["cursor"]["nextBatch"].Array();
    ASSERT_EQ(nextBatch.size(), 1U);
    ASSERT_BSONOBJ_EQ(nextBatch[0].embeddedObject(), BSON("_id" << 4));
}

TEST(OpMsg, ServerHandlesExhaustGetMoreCorrectly) {
    exhaustGetMoreTest(false);
}

TEST(OpMsg, ServerHandlesExhaustGetMoreCorrectlyWithChecksum) {
    exhaustGetMoreTest(true);
}

TEST(OpMsg, FindIgnoresExhaust) {
    auto conn = getIntegrationTestConnection();

    // Only test exhaust against a standalone and mongos.
    if (conn->isReplicaSetMember()) {
        return;
    }

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

    conn->dropCollection(nss);

    // Insert a few documents.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss, BSON("_id" << i));
    }

    // Issue a find request with exhaust flag. Returns 0 documents.
    auto findCmd = BSON("find" << nss.coll() << "batchSize" << 0);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), findCmd);
    auto request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    // The response should not have set moreToCome. We only expect getMore response to set
    // 'moreToCome'.
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
}

TEST(OpMsg, ServerDoesNotSetMoreToComeOnErrorInGetMore) {
    auto conn = getIntegrationTestConnection();

    // Only test exhaust against a standalone and mongos.
    if (conn->isReplicaSetMember()) {
        return;
    }

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

    conn->dropCollection(nss);

    // Insert a few documents.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss, BSON("_id" << i));
    }

    // Issue a find request to open a cursor but return 0 documents.
    auto findCmd = BSON("find" << nss.coll() << "batchSize" << 0);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), findCmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    const long long cursorId = res["cursor"]["id"].numberLong();
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    // Drop the collection, so that the next getMore will error.
    conn->dropCollection(nss);

    // Construct getMore request with exhaust flag.
    int batchSize = 2;
    GetMoreCommandRequest getMoreRequest(cursorId, std::string{nss.coll()});
    getMoreRequest.setBatchSize(batchSize);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), getMoreRequest.toBSON());
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run getMore. This should not start an exhaust stream.
    reply = conn->call(request);
    // The response should not have set moreToCome.
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_NOT_OK(getStatusFromCommandResult(res));
}

TEST(OpMsg, ExhaustWorksForAggCursor) {
    auto conn = getIntegrationTestConnection();

    // Only test exhaust against a standalone and mongos.
    if (conn->isReplicaSetMember()) {
        return;
    }

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");

    conn->dropCollection(nss);

    // Insert 5 documents so that a cursor using a batchSize of 2 requires three batches to get all
    // the results.
    for (int i = 0; i < 5; i++) {
        conn->insert(nss, BSON("_id" << i));
    }

    // Issue an agg request to open a cursor but return 0 documents. Specify a sort in order to
    // guarantee their return order.
    auto aggCmd = BSON("aggregate" << nss.coll() << "cursor" << BSON("batchSize" << 0) << "pipeline"
                                   << BSON_ARRAY(BSON("$sort" << BSON("_id" << 1))));
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), aggCmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    const long long cursorId = res["cursor"]["id"].numberLong();
    ASSERT(res["cursor"]["firstBatch"].Array().empty());
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    // Construct getMore request with exhaust flag. Set batch size so we will need multiple batches
    // to exhaust the cursor.
    int batchSize = 2;
    GetMoreCommandRequest getMoreRequest(cursorId, std::string{nss.coll()});
    getMoreRequest.setBatchSize(batchSize);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, nss.dbName(), getMoreRequest.toBSON());
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
    reply = conn->call(request);
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, cursorId, {BSON("_id" << 0), BSON("_id" << 1)});

    // Receive next exhaust batch.
    reply = conn->recv(lastRequestId);
    lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, cursorId, {BSON("_id" << 2), BSON("_id" << 3)});

    // Receive terminal batch.
    reply = conn->recv(lastRequestId);
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    assertNextBatch(reply, 0, {BSON("_id" << 4)});
}

TEST(OpMsg, ServerHandlesExhaustIsMasterCorrectly) {
    auto swConn = unittest::getFixtureConnectionString().connect("integration_test");
    uassertStatusOK(swConn.getStatus());
    auto fixtureConn = std::move(swConn.getValue());
    DBClientBase* conn = fixtureConn.get();

    if (fixtureConn->isReplicaSetMember()) {
        // Connect directly to the primary.
        conn = &static_cast<DBClientReplicaSet*>(fixtureConn.get())->primaryConn();
        ASSERT(conn);
    }

    auto clockSource = getGlobalServiceContext()->getPreciseClockSource();

    // Issue an isMaster command without a topology version.
    auto isMasterCmd = BSON("isMaster" << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, isMasterCmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    // Construct isMaster command with topologyVersion, maxAwaitTimeMS, and exhaust.
    isMasterCmd =
        BSON("isMaster" << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, isMasterCmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run isMaster command to initiate the exhaust stream.
    auto beforeExhaustCommand = clockSource->now();
    reply = conn->call(request);
    auto afterFirstResponse = clockSource->now();
    // Allow for clock skew when testing the response time.
    ASSERT_GT(duration_cast<Milliseconds>(afterFirstResponse - beforeExhaustCommand),
              Milliseconds(50));
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto nextTopologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT_BSONOBJ_EQ(topologyVersion, nextTopologyVersion);

    // Receive next exhaust message.
    auto lastRequestId = reply.header().getId();
    reply = conn->recv(lastRequestId);
    auto afterSecondResponse = clockSource->now();
    // Allow for clock skew when testing the response time.
    ASSERT_GT(duration_cast<Milliseconds>(afterSecondResponse - afterFirstResponse),
              Milliseconds(50));
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    nextTopologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT_BSONOBJ_EQ(topologyVersion, nextTopologyVersion);

    // The exhaust stream would continue indefinitely.
}

TEST(OpMsg, ServerHandlesExhaustIsMasterWithTopologyChange) {
    auto swConn = unittest::getFixtureConnectionString().connect("integration_test");
    uassertStatusOK(swConn.getStatus());
    auto fixtureConn = std::move(swConn.getValue());
    DBClientBase* conn = fixtureConn.get();

    if (fixtureConn->isReplicaSetMember()) {
        // Connect directly to the primary.
        conn = &static_cast<DBClientReplicaSet*>(fixtureConn.get())->primaryConn();
        ASSERT(conn);
    }

    auto clockSource = getGlobalServiceContext()->getPreciseClockSource();

    // Issue an isMaster command without a topology version.
    auto isMasterCmd = BSON("isMaster" << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, isMasterCmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    // Construct isMaster command with topologyVersion, maxAwaitTimeMS, and exhaust. Use a different
    // processId for the topologyVersion so that the first response is returned immediately.
    isMasterCmd = BSON("isMaster" << 1 << "topologyVersion"
                                  << BSON("processId" << OID::gen() << "counter" << 0LL)
                                  << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, isMasterCmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run isMaster command to initiate the exhaust stream. The first response should be received
    // immediately.
    auto beforeExhaustCommand = clockSource->now();
    reply = conn->call(request);
    auto afterFirstResponse = clockSource->now();
    // Allow for clock skew when testing the response time.
    ASSERT_LT(duration_cast<Milliseconds>(afterFirstResponse - beforeExhaustCommand),
              Milliseconds(50));
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto nextTopologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT_BSONOBJ_EQ(topologyVersion, nextTopologyVersion);

    // Receive next exhaust message. The second response waits for 'maxAwaitTimeMS'.
    auto lastRequestId = reply.header().getId();
    reply = conn->recv(lastRequestId);
    auto afterSecondResponse = clockSource->now();
    // Allow for clock skew when testing the response time.
    ASSERT_GT(duration_cast<Milliseconds>(afterSecondResponse - afterFirstResponse),
              Milliseconds(50));
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    nextTopologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT_BSONOBJ_EQ(topologyVersion, nextTopologyVersion);

    // The exhaust stream would continue indefinitely.
}

TEST(OpMsg, ServerRejectsExhaustIsMasterWithoutMaxAwaitTimeMS) {
    auto swConn = unittest::getFixtureConnectionString().connect("integration_test");
    uassertStatusOK(swConn.getStatus());
    auto fixtureConn = std::move(swConn.getValue());
    DBClientBase* conn = fixtureConn.get();

    if (fixtureConn->isReplicaSetMember()) {
        // Connect directly to the primary.
        conn = &static_cast<DBClientReplicaSet*>(fixtureConn.get())->primaryConn();
        ASSERT(conn);
    }

    // Issue an isMaster command with exhaust but no maxAwaitTimeMS.
    auto isMasterCmd = BSON("isMaster" << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, isMasterCmd);
    auto request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_NOT_OK(getStatusFromCommandResult(res));
}

void serverStatusCorrectlyShowsExhaustMetrics(std::string commandName) {
    auto conn = getIntegrationTestConnection();

    if (conn->isReplicaSetMember()) {
        // Don't run on replica sets as the RSM will use the streamable hello or isMaster protocol
        // by default. This can cause inconsistencies in our metrics tests.
        return;
    }

    bool useLegacyCommandName = (commandName != "hello");
    // Wait for stale exhaust streams to finish closing before testing the exhaust metrics.
    ASSERT(waitForCondition([&] {
        auto serverStatusCmd = BSON("serverStatus" << 1);
        BSONObj serverStatusReply;
        ASSERT(conn->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
        return serverStatusReply["connections"]["exhaustIsMaster"].numberInt() == 0 &&
            serverStatusReply["connections"]["exhaustHello"].numberInt() == 0;
    }));

    // Issue a hello or isMaster command without a topology version.
    auto cmd = BSON(commandName << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    cmd = BSON(commandName << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run hello or isMaster command to initiate the exhaust stream.
    reply = conn->call(request);
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // Start a new connection to the server to check the serverStatus metrics.
    auto conn2 =
        std::move(unittest::getFixtureConnectionString().connect("integration_test").getValue());
    uassert(ErrorCodes::SocketException, "connection failed", conn2);

    auto serverStatusCmd = BSON("serverStatus" << 1);
    BSONObj serverStatusReply;
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }
    // The exhaust stream would continue indefinitely.
}

TEST(OpMsg, ServerStatusCorrectlyShowsExhaustIsMasterMetrics) {
    return serverStatusCorrectlyShowsExhaustMetrics("isMaster");
}

TEST(OpMsg, ServerStatusCorrectlyShowsExhaustHelloMetrics) {
    return serverStatusCorrectlyShowsExhaustMetrics("hello");
}

TEST(OpMsg, ServerStatusCorrectlyShowsExhaustIsMasterMetricsWithIsMasterAlias) {
    return serverStatusCorrectlyShowsExhaustMetrics("ismaster");
}

void exhaustMetricSwitchingCommandNames(bool useLegacyCommandNameAtStart) {
    const auto conn1AppName = "integration_test";
    auto swConn1 = unittest::getFixtureConnectionString().connect(conn1AppName);
    uassertStatusOK(swConn1.getStatus());
    auto conn1 = std::move(swConn1.getValue());

    if (conn1->isReplicaSetMember()) {
        // Don't run on replica sets as the RSM will use the streamable hello or isMaster protocol
        // by default. This can cause inconsistencies in our metrics tests.
        return;
    }

    // Wait for stale exhaust streams to finish closing before testing the exhaust metrics.
    ASSERT(waitForCondition([&] {
        auto serverStatusCmd = BSON("serverStatus" << 1);
        BSONObj serverStatusReply;
        ASSERT(conn1->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
        return serverStatusReply["connections"]["exhaustIsMaster"].numberInt() == 0 &&
            serverStatusReply["connections"]["exhaustHello"].numberInt() == 0;
    }));

    // Issue a hello or isMaster command without a topology version.
    std::string cmdName = "hello";
    if (useLegacyCommandNameAtStart) {
        cmdName = "isMaster";
    }
    // Issue a hello or isMaster command without a topology version.
    auto cmd = BSON(cmdName << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn1->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    cmd = BSON(cmdName << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run hello or isMaster command to initiate the exhaust stream.
    reply = conn1->call(request);
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // Start a new connection to the server to check the serverStatus metrics.
    auto conn2 =
        std::move(unittest::getFixtureConnectionString().connect("integration_test2").getValue());
    uassert(ErrorCodes::SocketException, "connection failed", conn2);

    std::string threadName;
    ASSERT(waitForCondition([&] {
        threadName = getThreadNameByAppName(conn2.get(), conn1AppName);
        return !threadName.empty();
    }));

    auto serverStatusCmd = BSON("serverStatus" << 1);
    BSONObj serverStatusReply;
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandNameAtStart) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    const auto failPointObj =
        BSON("configureFailPoint" << "failCommand"
                                  << "mode" << BSON("times" << 1) << "data"
                                  << BSON("threadName" << threadName << "errorCode"
                                                       << ErrorCodes::NotWritablePrimary
                                                       << "failCommands" << BSON_ARRAY(cmdName)));
    auto response = conn2->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, failPointObj));
    ASSERT_OK(getStatusFromCommandResult(response->getCommandReply()));

    // Wait for the exhaust stream to close from the error returned by hello or isMaster.
    ASSERT(waitForCondition([&] {
        reply = conn1->recv(lastRequestId);
        lastRequestId = reply.header().getId();
        res = OpMsg::parse(reply).body;
        return !getStatusFromCommandResult(res).isOK();
    }));

    // Terminating the exhaust stream should not decrement the number of exhaust connections.
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandNameAtStart) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    // running a different command on conn1 to initiate a new exhaust stream.
    std::string newCmdName = "isMaster";
    if (useLegacyCommandNameAtStart) {
        newCmdName = "hello";
    }
    std::cout << newCmdName;
    auto newCmd =
        BSON(newCmdName << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, newCmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    reply = conn1->call(request);
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // exhaust metric should decrease for the exhaust type that was closed, and increase for the
    // exhaust type that was just opened.
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandNameAtStart) {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }
}

TEST(OpMsg, ExhaustIsMasterMetricSwitchingCommandNames) {
    return exhaustMetricSwitchingCommandNames(true);
}

TEST(OpMsg, ExhaustHelloMetricSwitchingCommandNames) {
    return exhaustMetricSwitchingCommandNames(false);
}


void exhaustMetricDecrementsOnNewOpAfterTerminatingExhaustStream(bool useLegacyCommandName) {
    const auto conn1AppName = "integration_test";
    auto swConn1 = unittest::getFixtureConnectionString().connect(conn1AppName);
    uassertStatusOK(swConn1.getStatus());
    auto conn1 = std::move(swConn1.getValue());

    if (conn1->isReplicaSetMember()) {
        // Don't run on replica sets as the RSM will use the streamable hello or isMaster protocol
        // by default. This can cause inconsistencies in our metrics tests.
        return;
    }

    // Wait for stale exhaust streams to finish closing before testing the exhaust metrics.
    ASSERT(waitForCondition([&] {
        auto serverStatusCmd = BSON("serverStatus" << 1);
        BSONObj serverStatusReply;
        ASSERT(conn1->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
        return serverStatusReply["connections"]["exhaustIsMaster"].numberInt() == 0 &&
            serverStatusReply["connections"]["exhaustHello"].numberInt() == 0;
    }));

    // Issue a hello or isMaster command without a topology version.
    std::string cmdName = "hello";
    if (useLegacyCommandName) {
        cmdName = "isMaster";
    }
    auto cmd = BSON(cmdName << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn1->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    cmd = BSON(cmdName << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run hello or isMaster command to initiate the exhaust stream.
    reply = conn1->call(request);
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // Start a new connection to the server to check the serverStatus metrics.
    auto conn2 =
        std::move(unittest::getFixtureConnectionString().connect("integration_test2").getValue());
    uassert(ErrorCodes::SocketException, "connection 2 failed", conn2);

    std::string threadName;
    ASSERT(waitForCondition([&] {
        threadName = getThreadNameByAppName(conn2.get(), conn1AppName);
        return !threadName.empty();
    }));

    auto serverStatusCmd = BSON("serverStatus" << 1);
    BSONObj serverStatusReply;
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    const auto failPointObj =
        BSON("configureFailPoint" << "failCommand"
                                  << "mode" << BSON("times" << 1) << "data"
                                  << BSON("threadName" << threadName << "errorCode"
                                                       << ErrorCodes::NotWritablePrimary
                                                       << "failCommands" << BSON_ARRAY(cmdName)));
    auto response = conn2->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, failPointObj));
    ASSERT_OK(getStatusFromCommandResult(response->getCommandReply()));

    // Wait for the exhaust stream to close from the error returned by hello or isMaster.
    ASSERT(waitForCondition([&] {
        reply = conn1->recv(lastRequestId);
        lastRequestId = reply.header().getId();
        res = OpMsg::parse(reply).body;
        return !getStatusFromCommandResult(res).isOK();
    }));

    // Terminating the exhaust stream should not decrement the number of exhaust connections.
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    // exhaust metric should now decrement after calling serverStatus on the connection that used
    // to have the exhaust stream.
    ASSERT(conn1->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
    ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
}

TEST(OpMsg, ExhaustIsMasterMetricDecrementsOnNewOpAfterTerminatingExhaustStream) {
    return exhaustMetricDecrementsOnNewOpAfterTerminatingExhaustStream(true);
}

TEST(OpMsg, ExhaustHelloMetricDecrementsOnNewOpAfterTerminatingExhaustStream) {
    return exhaustMetricDecrementsOnNewOpAfterTerminatingExhaustStream(false);
}

void exhaustMetricOnNewExhaustAfterTerminatingExhaustStream(bool useLegacyCommandName) {
    const auto conn1AppName = "integration_test";
    auto swConn1 = unittest::getFixtureConnectionString().connect(conn1AppName);
    uassertStatusOK(swConn1.getStatus());
    auto conn1 = std::move(swConn1.getValue());

    if (conn1->isReplicaSetMember()) {
        // Don't run on replica sets as the RSM will use the streamable hello or isMaster protocol
        // by default. This can cause inconsistencies in our metrics tests.
        return;
    }

    // Wait for stale exhaust streams to finish closing before testing the exhaust metrics.
    ASSERT(waitForCondition([&] {
        auto serverStatusCmd = BSON("serverStatus" << 1);
        BSONObj serverStatusReply;
        ASSERT(conn1->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
        return serverStatusReply["connections"]["exhaustIsMaster"].numberInt() == 0 &&
            serverStatusReply["connections"]["exhaustHello"].numberInt() == 0;
    }));

    // Issue a hello or isMaster command without a topology version.
    std::string cmdName = "hello";
    if (useLegacyCommandName) {
        cmdName = "isMaster";
    }
    auto cmd = BSON(cmdName << 1);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    auto request = opMsgRequest.serialize();

    Message reply = conn1->call(request);
    auto res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));
    auto topologyVersion = res["topologyVersion"].Obj().getOwned();
    ASSERT(!OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));

    cmd = BSON(cmdName << 1 << "topologyVersion" << topologyVersion << "maxAwaitTimeMS" << 100);
    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run hello or isMaster command to initiate the exhaust stream.
    reply = conn1->call(request);
    auto lastRequestId = reply.header().getId();
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // Start a new connection to the server to check the serverStatus metrics.
    auto conn2 =
        std::move(unittest::getFixtureConnectionString().connect("integration_test2").getValue());
    uassert(ErrorCodes::SocketException, "connection failed", conn2);

    std::string threadName;
    ASSERT(waitForCondition([&] {
        threadName = getThreadNameByAppName(conn2.get(), conn1AppName);
        return !threadName.empty();
    }));

    auto serverStatusCmd = BSON("serverStatus" << 1);
    BSONObj serverStatusReply;
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    const auto failPointObj =
        BSON("configureFailPoint" << "failCommand"
                                  << "mode" << BSON("times" << 1) << "data"
                                  << BSON("threadName" << threadName << "errorCode"
                                                       << ErrorCodes::NotWritablePrimary
                                                       << "failCommands" << BSON_ARRAY(cmdName)));
    auto response = conn2->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, failPointObj));
    ASSERT_OK(getStatusFromCommandResult(response->getCommandReply()));

    // Wait for the exhaust stream to close from the error returned by hello or isMaster.
    ASSERT(waitForCondition([&] {
        reply = conn1->recv(lastRequestId);
        lastRequestId = reply.header().getId();
        res = OpMsg::parse(reply).body;
        return !getStatusFromCommandResult(res).isOK();
    }));

    // Terminating the exhaust stream should not decrement the number of exhaust connections.
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }

    opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, cmd);
    request = opMsgRequest.serialize();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);

    // Run hello or isMaster command on conn1 to initiate a new exhaust stream.
    reply = conn1->call(request);
    ASSERT(OpMsg::isFlagSet(reply, OpMsg::kMoreToCome));
    res = OpMsg::parse(reply).body;
    ASSERT_OK(getStatusFromCommandResult(res));

    // exhaust metric should not increment or decrement after initiating a new exhaust stream.
    ASSERT(conn2->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatusReply));
    if (useLegacyCommandName) {
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustHello"].numberInt());
    } else {
        ASSERT_EQUALS(0, serverStatusReply["connections"]["exhaustIsMaster"].numberInt());
        ASSERT_EQUALS(1, serverStatusReply["connections"]["exhaustHello"].numberInt());
    }
}

TEST(OpMsg, ExhaustIsMasterMetricOnNewExhaustIsMasterAfterTerminatingExhaustStream) {
    return exhaustMetricOnNewExhaustAfterTerminatingExhaustStream(true);
}

TEST(OpMsg, ExhaustHelloMetricOnNewExhaustHelloAfterTerminatingExhaustStream) {
    return exhaustMetricOnNewExhaustAfterTerminatingExhaustStream(false);
}

TEST(OpMsg, ExhaustWithDBClientCursorBehavesCorrectly) {
    // This test simply tries to verify that using the exhaust option with DBClientCursor works
    // correctly. The externally visible behavior should technically be the same as a non-exhaust
    // cursor. The exhaust cursor should ideally provide a performance win over non-exhaust, but we
    // don't measure that here.
    auto conn = getIntegrationTestConnection();

    // Only test exhaust against a standalone and mongos.
    if (conn->isReplicaSetMember()) {
        return;
    }

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    conn->dropCollection(nss);

    const int nDocs = 5;
    LOGV2(22634, "Inserting {nDocs} documents.", "nDocs"_attr = nDocs);
    for (int i = 0; i < nDocs; i++) {
        auto doc = BSON("_id" << i);
        conn->insert(nss, doc);
    }

    ASSERT_EQ(conn->count(nss), size_t(nDocs));
    LOGV2(22635, "Finished document insertion.");

    // Open an exhaust cursor.
    FindCommandRequest findCmd{nss};
    findCmd.setSort(BSON("_id" << 1));
    findCmd.setBatchSize(2);
    auto cursor = conn->find(std::move(findCmd), ReadPreferenceSetting{}, ExhaustMode::kOn);

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

void checksumTest(bool enableChecksum) {
    // The server replies with a checksum if and only if the request has a checksum.
    auto conn = getIntegrationTestConnection();

    if (!enableChecksum) {
        disableClientChecksum();
    }

    ON_BLOCK_EXIT([&] { enableClientChecksum(); });

    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, BSON("ping" << 1));
    auto request = opMsgRequest.serialize();

    Message reply = conn->call(request);

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
    auto conn = getIntegrationTestConnection();

    auto buildInfo =
        conn->runCommand(OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                                     DatabaseName::kAdmin,
                                                     BSON("buildInfo" << 1)))
            ->getCommandReply();
    ASSERT_OK(getStatusFromCommandResult(buildInfo));
    const std::string bigData(kOpMsgReplyBSONBufferMaxSize + (1024), ' ');
    BSONObjBuilder bob;
    bob << "ismaster" << 1 << "ignoredField" << bigData << "$db"
        << "admin";
    OpMsgRequest request;
    request.body = bob.obj<BSONObj::LargeSizeTrait>();
    ASSERT_GT(request.body.objsize(), kOpMsgReplyBSONBufferMaxSize);
    auto requestMsg = request.serializeWithoutSizeChecking();

    Message replyMsg = conn->call(requestMsg);

    auto reply = OpMsg::parse(replyMsg);
    auto replyStatus = getStatusFromCommandResult(reply.body);
    ASSERT_NOT_OK(replyStatus);
    ASSERT_EQ(replyStatus, ErrorCodes::BSONObjectTooLarge);
}

class HelloOkTest final {
public:
    auto connect(boost::optional<bool> helloOk = boost::none) const {
        auto connStr = unittest::getFixtureConnectionString();

        auto swURI = MongoURI::parse(connStr.toString());
        ASSERT_OK(swURI.getStatus());

        auto uri = swURI.getValue();
        if (helloOk.has_value()) {
            uri.setHelloOk(helloOk.value());
        }

        auto swConn = connStr.connect(_appName, 0, &uri);
        uassertStatusOK(swConn.getStatus());
        auto conn = std::move(swConn.getValue());
        uassert(ErrorCodes::SocketException, "connection failed", conn);

        _configureFailPoint(conn.get(), conn->isMongos());
        return conn;
    }

    auto checkIfClientSupportsHello(DBClientBase* conn) const {
        auto checkHelloSupport = [conn](const std::string& helloCommand) {
            auto response = conn->runCommand(OpMsgRequestBuilder::create(
                                                 auth::ValidatedTenancyScope::kNotRequired,
                                                 DatabaseName::kAdmin,
                                                 BSON(helloCommand << 1)))
                                ->getCommandReply()
                                .getOwned();
            auto helloOk = response.getField("clientSupportsHello");
            ASSERT(!helloOk.eoo());
            return helloOk.Bool();
        };

        auto helloOk = checkHelloSupport("hello");
        ASSERT_EQ(helloOk, checkHelloSupport("isMaster"));
        ASSERT_EQ(helloOk, checkHelloSupport("ismaster"));
        return helloOk;
    }

private:
    void _configureFailPoint(DBClientBase* conn, bool isRouter) const {
        const auto threadName = getThreadNameByAppName(conn, _appName);
        // failpoint has a different name on the router
        StringData failPointName =
            isRouter ? "routerAppendHelloOkToHelloResponse" : "appendHelloOkToHelloResponse";
        const auto failPointObj =
            BSON("configureFailPoint" << failPointName << "mode"
                                      << "alwaysOn"
                                      << "data" << BSON("threadName" << threadName));
        auto response = conn->runCommand(OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, failPointObj));
        ASSERT_OK(getStatusFromCommandResult(response->getCommandReply()));
    }

    static constexpr auto _appName = "integration_test";
};

TEST(OpMsg, HelloOkIsDisabledByDefault) {
    HelloOkTest instance;
    auto conn = instance.connect();
    auto isHelloOk = instance.checkIfClientSupportsHello(conn.get());
    ASSERT(!isHelloOk);
}

TEST(OpMsg, HelloOkCanBeEnabled) {
    HelloOkTest instance;
    auto conn = instance.connect(true);
    auto isHelloOk = instance.checkIfClientSupportsHello(conn.get());
    ASSERT(isHelloOk);
}

TEST(OpMsg, HelloOkCanBeDisabled) {
    HelloOkTest instance;
    auto conn = instance.connect(false);
    auto isHelloOk = instance.checkIfClientSupportsHello(conn.get());
    ASSERT(!isHelloOk);
}

}  // namespace
}  // namespace mongo
