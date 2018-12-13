
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

#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * This DBClientConnection mock allows us to intercept outgoing client requests and to simulate a
 * remote server's responses to these requests. We use this to test DBClientCursor, since a
 * DBClientConnection is how a DBClientCursor interacts with the rest of the world. By mocking the
 * behavior of the DBClientConnection (i.e. the network) we can make sure that DBClientCursor
 * behaves correctly, assuming correct behavior of DBClientConnection.
 */
class DBClientConnectionForTest : public DBClientConnection {
public:
    DBClientConnectionForTest() {
        _setServerRPCProtocols(rpc::supports::kAll);       // allow all protocol types by default.
        _serverAddress = HostAndPort("localhost", 27017);  // dummy server address.
    }

    bool call(Message& toSend,
              Message& response,
              bool assertOk,
              std::string* actualServer) override {

        // Intercept request.
        const auto reqId = nextMessageId();
        toSend.header().setId(reqId);
        toSend.header().setResponseToMsgId(0);
        _lastSent = toSend;

        // Mock response.
        response = _mockCallResponse;
        response.header().setId(nextMessageId());
        response.header().setResponseToMsgId(reqId);

        return true;
    }

    bool recv(Message& m, int lastRequestId) override {
        m = _mockRecvResponse;
        return true;
    }

    // No-op.
    void killCursor(const NamespaceString& ns, long long cursorID) override {
        unittest::log() << "Killing cursor in DBClientConnectionForTest";
    }

    void setSupportedProtocols(rpc::ProtocolSet protocols) {
        _setServerRPCProtocols(protocols);
    }

    void setCallResponse(Message reply) {
        _mockCallResponse = reply;
    }

    void setRecvResponse(Message reply) {
        _mockRecvResponse = reply;
    }

    Message getLastSentMessage() {
        return _lastSent;
    }

    void clearLastSentMessage() {
        _lastSent = Message();
    }

private:
    Message _mockCallResponse;
    Message _mockRecvResponse;
    Message _lastSent;
};

class DBClientCursorTest : public unittest::Test {
protected:
    /**
     * An OP_MSG response to a 'find' command.
     */
    Message mockFindResponse(NamespaceString nss,
                             long long cursorId,
                             std::vector<BSONObj> firstBatch) {
        auto cursorRes = CursorResponse(nss, cursorId, firstBatch);
        return OpMsg{cursorRes.toBSON(CursorResponse::ResponseType::InitialResponse)}.serialize();
    }

    /**
     * An OP_MSG response to a 'getMore' command.
     */
    Message mockGetMoreResponse(NamespaceString nss,
                                long long cursorId,
                                std::vector<BSONObj> batch) {
        auto cursorRes = CursorResponse(nss, cursorId, batch);
        return OpMsg{cursorRes.toBSON(CursorResponse::ResponseType::SubsequentResponse)}
            .serialize();
    }
    /**
     * A generic non-ok OP_MSG command response.
     */
    Message mockErrorResponse(ErrorCodes::Error err) {
        OpMsgBuilder builder;
        BSONObjBuilder bodyBob;
        bodyBob.append("ok", 0);
        bodyBob.append("code", err);
        builder.setBody(bodyBob.done());
        return builder.finish();
    }

    BSONObj docObj(int id) {
        return BSON("_id" << id);
    }
};

TEST_F(DBClientCursorTest, DBClientCursorHandlesOpMsgExhaustCorrectly) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(
        &conn, NamespaceStringOrUUID(nss), Query().obj, 0, 0, nullptr, QueryOption_Exhaust, 0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), 0U);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);

    // Create a 'getMore' response and set it as the mock response. The 'moreToCome' flag is set on
    // the response to indicate this is an exhaust message.
    cursor.setBatchSize(2);
    std::vector<BSONObj> docs = {docObj(1), docObj(2)};
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, docs);
    OpMsg::setFlag(&getMoreResponseMsg, OpMsg::kMoreToCome);
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the first 'getMore' request with exhaust
    // flag set.
    conn.clearLastSentMessage();
    ASSERT(cursor.more());
    m = conn.getLastSentMessage();

    ASSERT(!m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());

    // Create and set a terminal 'getMore' response. The 'moreToCome' flag is not set, since this is
    // the last message of the stream.
    auto terminalDoc = BSON("_id"
                            << "terminal");
    auto getMoreTerminalResponseMsg = mockGetMoreResponse(nss, 0, {terminalDoc});
    conn.setRecvResponse(getMoreTerminalResponseMsg);

    // Request more results. This call to 'more' should not trigger a new 'getMore' request, since
    // the remote server should already be streaming results to us. After getting the final batch,
    // the cursor should be exhausted i.e. "dead".
    conn.clearLastSentMessage();
    ASSERT(cursor.more());
    ASSERT(conn.getLastSentMessage().empty());
    ASSERT_BSONOBJ_EQ(terminalDoc, cursor.next());
    ASSERT(cursor.isDead());
}

TEST_F(DBClientCursorTest, DBClientCursorResendsGetMoreIfMoreToComeFlagIsOmittedInExhaustMessage) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(
        &conn, NamespaceStringOrUUID(nss), Query().obj, 0, 0, nullptr, QueryOption_Exhaust, 0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);

    // Create and set a mock 'getMore' response. The 'moreToCome' flag is NOT set on the response,
    // even though exhaust mode was requested by the client. It is legal for a server to do this,
    // even if the cursor is not exhausted. In response, the client should resend a 'getMore' to
    // restart the stream.
    cursor.setBatchSize(2);
    std::vector<BSONObj> docs = {docObj(1), docObj(2)};
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, docs);
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the first 'getMore' request with exhaust
    // flag set.
    conn.clearLastSentMessage();
    ASSERT(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_EQ(msg.body["batchSize"].number(), 2);
    ASSERT(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());

    // Create another mock 'getMore' response. This one has the 'moreToCome' flag set.
    docs = {docObj(3), docObj(4)};
    getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, docs);
    OpMsg::setFlag(&getMoreResponseMsg, OpMsg::kMoreToCome);

    conn.setCallResponse(getMoreResponseMsg);

    // Request more results again. This call should trigger another 'getMore' request, since the
    // previous response had no 'moreToCome' flag set. This time the mock server will respond with
    // the 'moreToCome' flag set.
    conn.clearLastSentMessage();
    ASSERT(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(4), cursor.next());

    // Exhaust the cursor with a terminal 'getMore' response.
    auto terminalDoc = BSON("_id"
                            << "terminal");
    auto terminalGetMoreResponseMsg = mockGetMoreResponse(nss, 0, {terminalDoc});
    conn.setRecvResponse(terminalGetMoreResponseMsg);

    // Get the last returned document.
    conn.clearLastSentMessage();
    ASSERT(cursor.more());
    ASSERT_BSONOBJ_EQ(terminalDoc, cursor.next());

    // The cursor should now be exhausted.
    ASSERT(!cursor.more());
    ASSERT(cursor.isDead());
}

TEST_F(DBClientCursorTest, DBClientCursorMoreThrowsExceptionOnNonOKResponse) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(
        &conn, NamespaceStringOrUUID(nss), Query().obj, 0, 0, nullptr, QueryOption_Exhaust, 0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());

    // Create and set a mock error response.
    cursor.setBatchSize(2);
    auto errResponseMsg = mockErrorResponse(ErrorCodes::Interrupted);
    conn.setCallResponse(errResponseMsg);

    // Try to request more results, and expect an error.
    conn.clearLastSentMessage();
    ASSERT_THROWS_CODE(cursor.more(), DBException, ErrorCodes::Interrupted);
}

TEST_F(DBClientCursorTest, DBClientCursorMoreThrowsExceptionWhenMoreToComeFlagSetWithZeroCursorId) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(
        &conn, NamespaceStringOrUUID(nss), Query().obj, 0, 0, nullptr, QueryOption_Exhaust, 0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());

    // Create and set a getMore response that has the 'moreToCome' flag but also a cursor id of
    // zero.
    cursor.setBatchSize(2);
    auto getMoreResponseMsg = mockGetMoreResponse(nss, 0, {});
    OpMsg::setFlag(&getMoreResponseMsg, OpMsg::kMoreToCome);
    conn.setCallResponse(getMoreResponseMsg);

    // Try to request more results, and expect an error.
    conn.clearLastSentMessage();
    ASSERT_THROWS_CODE(cursor.more(), DBException, 50935);
}

TEST_F(DBClientCursorTest, DBClientCursorIgnoresExhaustForOpQueryMessages) {
    // Set up the DBClientCursor and a mock client connection. If we set the server RPC protocol to
    // OpQuery, then when we assemble a command request in DBClientCursor, we will make a command
    // style OpQuery request, as opposed to an OpMsg request. We want to make sure that for command
    // style OpQuery requests, we ignore the exhaust option, and that cursor queries work normally.
    DBClientConnectionForTest conn;
    conn.setSupportedProtocols(rpc::supports::kOpQueryOnly);

    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(
        &conn, NamespaceStringOrUUID(nss), Query().obj, 0, 0, nullptr, QueryOption_Exhaust, 0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    QueryMessage queryMsg(m);
    ASSERT_EQ(queryMsg.query.getStringField("find"), nss.coll());
    ASSERT_EQ(queryMsg.query["batchSize"].number(), 0);
    ASSERT_EQ(0, queryMsg.queryOptions);

    // Create and set a non-exhaust getMore response.
    cursor.setBatchSize(2);
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(1), docObj(2)});
    conn.setCallResponse(getMoreResponseMsg);

    // Trigger another 'getMore' request.
    conn.clearLastSentMessage();
    ASSERT(cursor.more());

    // Make sure the sent request has no exhaust query options set.
    m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    queryMsg = QueryMessage(m);
    ASSERT_EQ(queryMsg.query["getMore"].number(), cursorId);
    ASSERT_EQ(queryMsg.query["collection"].str(), nss.coll());
    ASSERT_EQ(0, queryMsg.queryOptions);
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
}

TEST_F(DBClientCursorTest, DBClientCursorPassesReadOnceFlag) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    DBClientCursor cursor(&conn,
                          NamespaceStringOrUUID(nss),
                          QUERY("query" << BSONObj() << "$readOnce" << true).obj,
                          0,
                          0,
                          nullptr,
                          /*QueryOption*/ 0,
                          0);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the 'find' request was sent with readOnce.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), 0U);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("readOnce")) << msg.body;
}

}  // namespace
}  // namespace mongo
