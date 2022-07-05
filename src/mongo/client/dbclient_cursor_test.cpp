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


#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


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
        _serverAddress = HostAndPort("localhost", 27017);  // dummy server address.
    }

    Status recv(Message& m, int lastRequestId) override {
        m = _mockRecvResponse;
        return Status::OK();
    }

    // No-op.
    void killCursor(const NamespaceString& ns, long long cursorID) override {
        LOGV2(20131, "Killing cursor in DBClientConnectionForTest");
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
    void _call(Message& toSend, Message& response, std::string* actualServer) override {

        // Intercept request.
        const auto reqId = nextMessageId();
        toSend.header().setId(reqId);
        toSend.header().setResponseToMsgId(0);
        OpMsg::appendChecksum(&toSend);
        _lastSent = toSend;

        // Mock response.
        response = _mockCallResponse;
        response.header().setId(nextMessageId());
        response.header().setResponseToMsgId(reqId);
        OpMsg::appendChecksum(&response);
    }

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

TEST_F(DBClientCursorTest, DBClientCursorCallsMetaDataReaderOncePerBatch) {
    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
    cursor.setBatchSize(2);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {docObj(1), docObj(2)});
    conn.setCallResponse(findResponseMsg);

    int numMetaRead = 0;
    conn.setReplyMetadataReader(
        [&](OperationContext* opCtx, const BSONObj& metadataObj, StringData target) {
            numMetaRead++;
            return Status::OK();
        });

    // Trigger a find command.
    ASSERT(cursor.init());

    // First batch from the initial find command.
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    // Test that the metadata reader callback is called once.
    ASSERT_EQ(1, numMetaRead);

    // Set a terminal getMore response with cursorId 0.
    auto getMoreResponseMsg = mockGetMoreResponse(nss, 0, {docObj(3), docObj(4)});
    conn.setCallResponse(getMoreResponseMsg);

    // Trigger a subsequent getMore command.
    ASSERT_TRUE(cursor.more());

    // Second batch from the getMore command.
    ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(4), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_TRUE(cursor.isDead());
    // Test that the metadata reader callback is called twice.
    ASSERT_EQ(2, numMetaRead);
}

TEST_F(DBClientCursorTest, DBClientCursorHandlesOpMsgExhaustCorrectly) {

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
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
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
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
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
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
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
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
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
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

TEST_F(DBClientCursorTest, DBClientCursorPassesReadOnceFlag) {
    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    findCmd.setReadOnce(true);
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
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
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("readOnce")) << msg.body;
}

// "Resume fields" refers to $_requestResumeToken and $_resumeAfter.
TEST_F(DBClientCursorTest, DBClientCursorPassesResumeFields) {
    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    findCmd.setRequestResumeToken(true);
    findCmd.setResumeAfter(BSON("$recordId" << 5LL));
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // Verify that the 'find' request was sent with $_requestResumeToken and $_resumeAfter.
    auto m = conn.getLastSentMessage();
    ASSERT(!m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("$_requestResumeToken")) << msg.body;
    ASSERT_TRUE(msg.body.hasField("$_resumeAfter")) << msg.body;
    ASSERT_TRUE(msg.body.getObjectField("$_resumeAfter").hasField("$recordId")) << msg.body;
    ASSERT_EQ(msg.body.getObjectField("$_resumeAfter")["$recordId"].numberLong(), 5LL) << msg.body;
}

TEST_F(DBClientCursorTest, DBClientCursorTailable) {
    // This tests DBClientCursor in tailable mode. Cases to test are:
    // 1. Initial find command has {tailable: true} set.
    // 2. A getMore can handle a normal batch correctly.
    // 3. A getMore can handle an empty batch correctly.
    // 4. The tailable cursor is not dead until the cursorId is 0 in the response.

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    findCmd.setTailable(true);
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // --- Test 1 ---
    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("tailable")) << msg.body;

    // --- Test 2 ---
    // Create a 'getMore' response with two documents and set it as the mock response.
    cursor.setBatchSize(2);
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(1), docObj(2)});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the first 'getMore' request.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());

    // --- Test 3 ---
    // Create a 'getMore' response with an empty batch and set it as the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the second 'getMore' request.
    // more() should return false on an empty batch.
    conn.clearLastSentMessage();
    ASSERT_FALSE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    // But the cursor should be still valid.
    ASSERT_FALSE(cursor.isDead());

    // --- Test 4 ---
    // Create a 'getMore' response with cursorId = 0 and set it as the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, 0, {docObj(3)});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the third 'getMore' request.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    // Cursor is dead if the cursorId is 0.
    ASSERT_TRUE(cursor.isDead());

    // Request more results. This should not trigger a new 'getMore' request as the cursor is dead.
    conn.clearLastSentMessage();
    ASSERT_FALSE(cursor.more());
    ASSERT_TRUE(conn.getLastSentMessage().empty());
}

TEST_F(DBClientCursorTest, DBClientCursorTailableAwaitData) {
    // This tests DBClientCursor in tailable awaitData mode. Cases to test are:
    // 1. Initial find command has {tailable: true} and {awaitData: true} set.
    // 2. A subsequent getMore command sets awaitData timeout correctly.

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    findCmd.setTailable(true);
    findCmd.setAwaitData(true);
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // --- Test 1 ---
    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("tailable")) << msg.body;
    ASSERT_TRUE(msg.body.getBoolField("awaitData")) << msg.body;

    cursor.setAwaitDataTimeoutMS(Milliseconds{5000});
    ASSERT_EQ(cursor.getAwaitDataTimeoutMS(), Milliseconds{5000});

    // --- Test 2 ---
    // Create a 'getMore' response with two documents and set it as the mock response.
    cursor.setBatchSize(2);
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(1), docObj(2)});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the first 'getMore' request.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    // Make sure the correct awaitData timeout is sent.
    ASSERT_EQ(msg.body["maxTimeMS"].number(), 5000);
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
}

TEST_F(DBClientCursorTest, DBClientCursorTailableAwaitDataExhaust) {
    // This tests DBClientCursor in tailable awaitData exhaust mode. Cases to test are:
    // 1. Initial find command has {tailable: true} and {awaitData: true} set.
    // 2. The first getMore command sets awaitData timeout correctly and has kExhaustSupported flag.
    // 3. Exhaust receive can handle a normal batch correctly.
    // 4. Exhaust receive can handle an empty batch correctly.
    // 5. Cursor resends a new 'getMore' command if the previous response had no 'moreToCome' flag.
    // 6. The tailable cursor is not dead until the cursorId is 0 in the response.

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss("test", "coll");
    FindCommandRequest findCmd{nss};
    findCmd.setTailable(true);
    findCmd.setAwaitData(true);
    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, true /*isExhaust*/);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // --- Test 1 ---
    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll());
    ASSERT_EQ(msg.body["batchSize"].number(), 0);
    ASSERT_TRUE(msg.body.getBoolField("tailable")) << msg.body;
    ASSERT_TRUE(msg.body.getBoolField("awaitData")) << msg.body;
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
    ASSERT_FALSE(cursor.connectionHasPendingReplies());

    // --- Test 2 ---
    cursor.setAwaitDataTimeoutMS(Milliseconds{5000});
    ASSERT_EQ(cursor.getAwaitDataTimeoutMS(), Milliseconds{5000});

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
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_EQ(msg.body["maxTimeMS"].number(), 5000);
    ASSERT_TRUE(OpMsg::isFlagSet(m, OpMsg::kExhaustSupported));
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
    ASSERT_TRUE(cursor.connectionHasPendingReplies());

    // --- Test 3 ---
    // Create a 'getMore' response with the 'moreToCome' flag set and set it as the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(3)});
    OpMsg::setFlag(&getMoreResponseMsg, OpMsg::kMoreToCome);
    conn.setRecvResponse(getMoreResponseMsg);

    // Request more results. But this should not trigger a new 'getMore' request because the
    // previous response had the 'moreToCome' flag set.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    ASSERT_TRUE(conn.getLastSentMessage().empty());
    ASSERT_BSONOBJ_EQ(docObj(3), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
    ASSERT_TRUE(cursor.connectionHasPendingReplies());

    // --- Test 4 ---
    // Create a 'getMore' response with an empty batch and the 'moreToCome' flag set and set it as
    // the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {});
    OpMsg::setFlag(&getMoreResponseMsg, OpMsg::kMoreToCome);
    conn.setRecvResponse(getMoreResponseMsg);

    // Request more results. But this should not trigger a new 'getMore' request because the
    // previous response had the 'moreToCome' flag set.
    conn.clearLastSentMessage();
    // more() should return false on an empty batch.
    ASSERT_FALSE(cursor.more());
    ASSERT_TRUE(conn.getLastSentMessage().empty());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    // But the cursor should be still valid.
    ASSERT_FALSE(cursor.isDead());
    ASSERT_TRUE(cursor.connectionHasPendingReplies());

    // --- Test 5 ---
    // Create a 'getMore' response without the 'moreToCome' flag and set it as the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(4)});
    conn.setRecvResponse(getMoreResponseMsg);

    // Request more results. But this should not trigger a new 'getMore' request because the
    // previous response had the 'moreToCome' flag set.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    ASSERT_TRUE(conn.getLastSentMessage().empty());
    ASSERT_BSONOBJ_EQ(docObj(4), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
    ASSERT_FALSE(cursor.connectionHasPendingReplies());

    // --- Test 6 ---
    // Create a 'getMore' response with cursorId 0 and set it as the mock response.
    getMoreResponseMsg = mockGetMoreResponse(nss, 0, {docObj(5)});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This should trigger a new 'getMore' request because the previous
    // response had no 'moreToCome' flag.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore");
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong);
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId);
    ASSERT_EQ(msg.body["maxTimeMS"].number(), 5000);
    ASSERT_BSONOBJ_EQ(docObj(5), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    // Cursor is dead if the cursorId is 0.
    ASSERT_TRUE(cursor.isDead());
    ASSERT_FALSE(cursor.connectionHasPendingReplies());

    // Request more results. This should not trigger a new 'getMore' request as the cursor is dead.
    conn.clearLastSentMessage();
    ASSERT_FALSE(cursor.more());
    ASSERT_TRUE(conn.getLastSentMessage().empty());
}

TEST_F(DBClientCursorTest, DBClientCursorOplogQuery) {
    // This tests DBClientCursor supports oplog query with special fields in the command request.
    // 1. Initial find command has "filter", "tailable", "awaitData", "maxTimeMS", "batchSize",
    //    "term" and "readConcern" fields set.
    // 2. A subsequent getMore command sets awaitData timeout and lastKnownCommittedOpTime
    //    correctly.

    // Set up the DBClientCursor and a mock client connection.
    DBClientConnectionForTest conn;
    const NamespaceString nss = NamespaceString::kRsOplogNamespace;
    const BSONObj filterObj = BSON("ts" << BSON("$gte" << Timestamp(123, 4)));
    const BSONObj readConcernObj = BSON("afterClusterTime" << Timestamp(0, 1));
    const long long maxTimeMS = 5000LL;
    const long long term = 5;

    FindCommandRequest findCmd{nss};
    findCmd.setFilter(filterObj);
    findCmd.setReadConcern(readConcernObj);
    findCmd.setMaxTimeMS(maxTimeMS);
    findCmd.setTerm(term);
    findCmd.setTailable(true);
    findCmd.setAwaitData(true);

    DBClientCursor cursor(&conn, findCmd, ReadPreferenceSetting{}, false);
    cursor.setBatchSize(0);

    // Set up mock 'find' response.
    const long long cursorId = 42;
    Message findResponseMsg = mockFindResponse(nss, cursorId, {});

    conn.setCallResponse(findResponseMsg);
    ASSERT(cursor.init());

    // --- Test 1 ---
    // Verify that the initial 'find' request was sent.
    auto m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    auto msg = OpMsg::parse(m);
    ASSERT_EQ(OpMsg::flags(m), OpMsg::kChecksumPresent);
    ASSERT_EQ(msg.body.getStringField("find"), nss.coll()) << msg.body;
    ASSERT_BSONOBJ_EQ(msg.body["filter"].Obj(), filterObj);
    ASSERT_TRUE(msg.body.getBoolField("tailable")) << msg.body;
    ASSERT_TRUE(msg.body.getBoolField("awaitData")) << msg.body;
    ASSERT_EQ(msg.body["maxTimeMS"].numberLong(), maxTimeMS) << msg.body;
    ASSERT_EQ(msg.body["batchSize"].number(), 0) << msg.body;
    ASSERT_EQ(msg.body["term"].numberLong(), term) << msg.body;
    ASSERT_BSONOBJ_EQ(msg.body["readConcern"].Obj(), readConcernObj);

    cursor.setAwaitDataTimeoutMS(Milliseconds{5000});
    ASSERT_EQ(cursor.getAwaitDataTimeoutMS(), Milliseconds{5000});

    cursor.setCurrentTermAndLastCommittedOpTime(term, repl::OpTime(Timestamp(123, 4), term));

    // --- Test 2 ---
    // Create a 'getMore' response with two documents and set it as the mock response.
    cursor.setBatchSize(2);
    auto getMoreResponseMsg = mockGetMoreResponse(nss, cursorId, {docObj(1), docObj(2)});
    conn.setCallResponse(getMoreResponseMsg);

    // Request more results. This call should trigger the first 'getMore' request.
    conn.clearLastSentMessage();
    ASSERT_TRUE(cursor.more());
    m = conn.getLastSentMessage();
    ASSERT_FALSE(m.empty());
    msg = OpMsg::parse(m);
    ASSERT_EQ(StringData(msg.body.firstElement().fieldName()), "getMore") << msg.body;
    ASSERT_EQ(msg.body["getMore"].type(), BSONType::NumberLong) << msg.body;
    ASSERT_EQ(msg.body["getMore"].numberLong(), cursorId) << msg.body;
    // Make sure the correct awaitData timeout is sent.
    ASSERT_EQ(msg.body["maxTimeMS"].number(), 5000) << msg.body;
    // Make sure the correct term is sent.
    ASSERT_EQ(msg.body["term"].numberLong(), term) << msg.body;
    // Make sure the correct lastKnownCommittedOpTime is sent.
    ASSERT_EQ(msg.body["lastKnownCommittedOpTime"]["ts"].timestamp(), Timestamp(123, 4))
        << msg.body;
    ASSERT_EQ(msg.body["lastKnownCommittedOpTime"]["t"].numberLong(), term) << msg.body;
    ASSERT_BSONOBJ_EQ(docObj(1), cursor.next());
    ASSERT_BSONOBJ_EQ(docObj(2), cursor.next());
    ASSERT_FALSE(cursor.moreInCurrentBatch());
    ASSERT_FALSE(cursor.isDead());
}

}  // namespace
}  // namespace mongo
