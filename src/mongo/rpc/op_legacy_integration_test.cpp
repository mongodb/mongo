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
#include "mongo/db/dbmessage.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

std::unique_ptr<DBClientBase> getIntegrationTestConnection() {
    auto swConn = unittest::getFixtureConnectionString().connect("op_legacy_integration_test");
    uassertStatusOK(swConn.getStatus());
    return std::move(swConn.getValue());
}

Message makeUnsupportedOpUpdateMessage(StringData ns, BSONObj query, BSONObj update, int flags) {
    return makeMessage(dbUpdate, [&](BufBuilder& b) {
        const int reservedFlags = 0;
        b.appendNum(reservedFlags);
        b.appendStr(ns);
        b.appendNum(flags);

        query.appendSelfToBufBuilder(b);
        update.appendSelfToBufBuilder(b);
    });
}

Message makeUnsupportedOpRemoveMessage(StringData ns, BSONObj query, int flags) {
    return makeMessage(dbDelete, [&](BufBuilder& b) {
        const int reservedFlags = 0;
        b.appendNum(reservedFlags);
        b.appendStr(ns);
        b.appendNum(flags);

        query.appendSelfToBufBuilder(b);
    });
}

Message makeUnsupportedOpKillCursorsMessage(long long cursorId) {
    return makeMessage(dbKillCursors, [&](BufBuilder& b) {
        b.appendNum((int)0);  // reserved
        b.appendNum((int)1);  // number
        b.appendNum(cursorId);
    });
}

Message makeUnsupportedOpQueryMessage(StringData ns,
                                      BSONObj query,
                                      int nToReturn,
                                      int nToSkip,
                                      const BSONObj* fieldsToReturn,
                                      int queryOptions) {
    return makeMessage(dbQuery, [&](BufBuilder& b) {
        b.appendNum(queryOptions);
        b.appendStr(ns);
        b.appendNum(nToSkip);
        b.appendNum(nToReturn);
        query.appendSelfToBufBuilder(b);
        if (fieldsToReturn)
            fieldsToReturn->appendSelfToBufBuilder(b);
    });
}

Message makeUnsupportedOpGetMoreMessage(StringData ns,
                                        long long cursorId,
                                        int nToReturn,
                                        int flags) {
    return makeMessage(dbGetMore, [&](BufBuilder& b) {
        b.appendNum(flags);
        b.appendStr(ns);
        b.appendNum(nToReturn);
        b.appendNum(cursorId);
    });
}

// Issue a find command request so we can use cursor id from it to test the unsupported getMore
// and killCursors wire protocol ops.
int64_t getValidCursorIdFromFindCmd(DBClientBase* conn, const char* collName) {
    Message findCmdRequest =
        OpMsgRequest::fromDBAndBody("testOpLegacy", BSON("find" << collName << "batchSize" << 2))
            .serialize();
    Message findCmdReply;
    conn->call(findCmdRequest, findCmdReply);
    BSONObj findCmdReplyBody = OpMsg::parse(findCmdReply).body;
    auto cr = CursorResponse::parseFromBSON(findCmdReplyBody.getOwned());
    ASSERT_OK(cr.getStatus());
    const int64_t cursorId = cr.getValue().getCursorId();
    ASSERT_NE(0, cursorId);

    return cursorId;
}

TEST(OpLegacy, GetLastError) {
    auto conn = getIntegrationTestConnection();

    static const auto getLastErrorCommand = fromjson(R"({"getlasterror": 1})");
    BSONObj replyObj;
    conn->runCommand(DatabaseName::kAdmin, getLastErrorCommand, replyObj);

    // 'getLastError' command is no longer supported and will always fail.
    auto status = getStatusFromCommandResult(replyObj);
    ASSERT_NOT_OK(status) << replyObj;
    ASSERT_EQ(status.code(), ErrorCodes::CommandNotFound) << replyObj;
}

TEST(OpLegacy, UnsupportedWriteOps) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "testOpLegacy.UnsupportedWriteOps";

    // Building parts for the unsupported requests.
    const BSONObj doc1 = fromjson("{a: 1}");
    const BSONObj doc2 = fromjson("{a: 2}");
    const BSONObj insert[2] = {doc1, doc2};
    const BSONObj query = fromjson("{a: {$lt: 42}}");
    const BSONObj update = fromjson("{$set: {b: 2}}");

    // Issue the requests. They are expected to fail.
    Message ignore;
    auto opInsert = makeUnsupportedOpInsertMessage(ns, insert, 2, 0 /*continue on error*/);
    ASSERT_THROWS(conn->call(opInsert, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opUpdate = makeUnsupportedOpUpdateMessage(ns, query, update, 0 /*no upsert, no multi*/);
    ASSERT_THROWS(conn->call(opUpdate, ignore), ExceptionForCat<ErrorCategory::NetworkError>);

    auto opDelete = makeUnsupportedOpRemoveMessage(ns, query, 0 /*limit*/);
    ASSERT_THROWS(conn->call(opDelete, ignore), ExceptionForCat<ErrorCategory::NetworkError>);
}

void assertFailure(const Message response, StringData expectedErr) {
    QueryResult::ConstView qr = response.singleData().view2ptr();
    BufReader responseData(qr.data(), qr.dataLen());
    BSONObj responseBody = responseData.read<BSONObj>();

    ASSERT_FALSE(responseBody["ok"].trueValue()) << responseBody;
    ASSERT_EQ(5739101, responseBody["code"].Int()) << responseBody;
    ASSERT_NE(getErrField(responseBody).checkAndGetStringData().find(expectedErr),
              std::string::npos)
        << responseBody;
}

TEST(OpLegacy, UnsupportedReadOps) {
    auto conn = getIntegrationTestConnection();
    const std::string ns = "testOpLegacy.UnsupportedReadOps";

    BSONObj insert = fromjson(R"({
        insert: "UnsupportedReadOps",
        documents: [ {a: 1},{a: 2},{a: 3},{a: 4},{a: 5},{a: 6},{a: 7} ]
    })");
    BSONObj ignoreResponse;
    ASSERT(conn->runCommand(DatabaseName::createDatabaseName_forTest(boost::none, "testOpLegacy"),
                            insert,
                            ignoreResponse));

    // Issue the unsupported requests. They all should fail one way or another.
    Message opQueryRequest = makeUnsupportedOpQueryMessage(ns,
                                                           fromjson("{}"),
                                                           2 /*nToReturn*/,
                                                           0 /*nToSkip*/,
                                                           nullptr /*fieldsToReturn*/,
                                                           0 /*queryOptions*/);
    Message opQueryReply;
    conn->call(opQueryRequest, opQueryReply);
    assertFailure(opQueryReply, "OP_QUERY is no longer supported");

    const int64_t cursorId = getValidCursorIdFromFindCmd(conn.get(), "UnsupportedReadOps");

    auto opGetMore = makeUnsupportedOpGetMoreMessage(ns, cursorId, 2 /*nToReturn*/, 0 /*flags*/);
    Message opGetMoreReply;
    conn->call(opGetMore, opGetMoreReply);
    assertFailure(opGetMoreReply, "OP_GET_MORE is no longer supported");

    auto opKillCursors = makeUnsupportedOpKillCursorsMessage(cursorId);
    Message opKillCursorsReply;
    ASSERT_THROWS(conn->call(opKillCursors, opKillCursorsReply),
                  ExceptionForCat<ErrorCategory::NetworkError>);
}

TEST(OpLegacy, GenericCommandViaOpQuery) {
    auto conn = getIntegrationTestConnection();

    // The actual command doesn't matter, as long as it's not 'hello' or 'isMaster'.
    auto opQuery = makeUnsupportedOpQueryMessage("testOpLegacy.$cmd",
                                                 fromjson("{serverStatus: 1}"),
                                                 1 /*nToReturn*/,
                                                 0 /*nToSkip*/,
                                                 nullptr /*fieldsToReturn*/,
                                                 0 /*queryOptions*/);
    Message replyQuery;
    conn->call(opQuery, replyQuery);
    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    BufReader data(qr.data(), qr.dataLen());
    BSONObj obj = data.read<BSONObj>();
    auto status = getStatusFromCommandResult(obj);
    ASSERT_EQ(status.code(), ErrorCodes::UnsupportedOpQueryCommand);
}

// Test commands that are still allowed via OP_QUERY protocol.
void testAllowedCommand(const char* command,
                        bool expectToBeCounted,
                        ErrorCodes::Error code = ErrorCodes::OK) {
    auto conn = getIntegrationTestConnection();

    auto opQuery = makeUnsupportedOpQueryMessage("testOpLegacy.$cmd",
                                                 fromjson(command),
                                                 1 /*nToReturn*/,
                                                 0 /*nToSkip*/,
                                                 nullptr /*fieldsToReturn*/,
                                                 0 /*queryOptions*/);

    auto serverStatusCmd = fromjson("{serverStatus: 1}");
    BSONObj serverStatus;
    ASSERT(conn->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatus));
    auto opCountersPrior = serverStatus["opcounters"]["deprecated"];
    const auto queryCountPrior = opCountersPrior ? opCountersPrior["query"].Long() : 0;

    Message replyQuery;
    conn->call(opQuery, replyQuery);
    QueryResult::ConstView qr = replyQuery.singleData().view2ptr();
    BufReader data(qr.data(), qr.dataLen());
    BSONObj obj = data.read<BSONObj>();
    auto status = getStatusFromCommandResult(obj);
    ASSERT_EQ(status.code(), code);

    ASSERT(conn->runCommand(DatabaseName::kAdmin, serverStatusCmd, serverStatus));
    auto opCounters = serverStatus["opcounters"]["deprecated"];
    const auto queryCount = opCounters ? opCounters["query"].Long() : 0;

    ASSERT_EQ(queryCountPrior + (expectToBeCounted ? 1 : 0), queryCount) << command;
}

TEST(OpLegacy, IsSelfCommandViaOpQuery) {
    testAllowedCommand("{_isSelf: 1}", true /* expectToBeCounted */);
}

TEST(OpLegacy, BuildinfoCommandViaOpQuery) {
    testAllowedCommand("{buildinfo: 1}", true /* expectToBeCounted */);
}

TEST(OpLegacy, BuildInfoCommandViaOpQuery) {
    testAllowedCommand("{buildInfo: 1}", true /* expectToBeCounted */);
}

TEST(OpLegacy, HelloCommandViaOpQuery) {
    testAllowedCommand("{hello: 1}", false /* expectToBeCounted */);
}

TEST(OpLegacy, IsMasterCommandViaOpQuery) {
    testAllowedCommand("{isMaster: 1}", false /* expectToBeCounted */);
}

TEST(OpLegacy, IsmasterCommandViaOpQuery) {
    testAllowedCommand("{ismaster: 1}", false /* expectToBeCounted */);
}

TEST(OpLegacy, SaslStartCommandViaOpQuery) {
    // Some older drivers continue to authenticate using OP_QUERY commands, even if the
    // isMaster/hello protocol negotiation resolves to OP_MSG. For this reason, the server must
    // continue to accept "saslStart" commands as OP_QUERY.
    //
    // Here we verify that "saslStart" command passes parsing since the request is actually an
    // invalid authentication request. The AuthenticationFailed error code means that it passes
    // request parsing.
    testAllowedCommand(R"({
                           saslStart: 1,
                           "mechanism":"SCRAM-SHA-256",
                           "options":{"skipEmptyExchange":true},
                           "payload":{
                               "$binary":{
                                   "base64":"biwsbj1fX3N5c3RlbSxyPUlyNDVmQm1WNWNuUXJSS3FhdU9JUERCTUhkV2NrK01i",
                                   "subType":"0"
                               }
                           }
                       })",
                       true /* expectToBeCounted */,
                       ErrorCodes::AuthenticationFailed);
}

TEST(OpLegacy, SaslContinueCommandViaOpQuery) {
    // Some older drivers continue to authenticate using OP_QUERY commands, even if the
    // isMaster/hello protocol negotiation resolves to OP_MSG. For this reason, the server must
    // continue to accept "saslContinue" commands as OP_QUERY.
    //
    // Here we verify that "saslContinue" command passes parsing since the request is actually an
    // invalid authentication request. The ProtocolError error code means that it passes request
    // parsing.
    testAllowedCommand(R"({
                           saslContinue: 1,
                           "payload":{
                               "$binary":{
                                   "base64":"Yz1iaXdzLHI9SXI0NWZCbVY1Y25RclJLcWF1T0lQREJNSGRXY2srTWJSNE81SnJrcnV4anorRDl2WXkrKzlnNlhBVHFCV0pMbSxwPUJTV3puZnNjcG8rYVhnc1YyT2xEa2NFSjF5NW9rM2xWSWQybjc4NlJ5MTQ9",
                                   "subType":"0"
                               }
                           },
                           "conversationId":1
                       })",
                       true /* expectToBeCounted */,
                       ErrorCodes::ProtocolError);
}

TEST(OpLegacy, AuthenticateCommandViaOpQuery) {
    // Some older drivers continue to authenticate using OP_QUERY commands, even if the
    // isMaster/hello protocol negotiation resolves to OP_MSG. For this reason, the server must
    // continue to accept "authenticate" commands as OP_QUERY.
    //
    // Here we only verify that "authenticate" command passes parsing since the request is actually
    // an invalid authentication request. The AuthenticationFailed error code means that it passes
    // request parsing.
    testAllowedCommand(R"({authenticate: 1, mechanism: "MONGODB-X509"})",
                       true /* expectToBeCounted */,
                       ErrorCodes::AuthenticationFailed);
}

}  // namespace
}  // namespace mongo
