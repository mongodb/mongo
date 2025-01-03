/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "command_helpers.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/database_name.h"
#include "mongo/rpc/op_msg.h"

namespace mongo::query_tester {
// Returns the array element containing result documents.
BSONObj getResultsFromCommandResponse(const BSONObj& cmdResponse, size_t testNum) {
    uassert(9670416,
            str::stream{} << "Failed to execute test number " << testNum
                          << ". Expected OK command result but got " << cmdResponse,
            cmdResponse.getField("ok").trueValue());
    // Assume format is correct and cursor is an object containing 'firstBatch' or 'nextBatch'.
    auto&& cursorObj = cmdResponse.getField("cursor").Obj();
    return cursorObj.getField(cursorObj.hasField("firstBatch") ? "firstBatch" : "nextBatch")
        .wrap("")
        .getOwned();
}

BSONObj runCommand(DBClientConnection* const conn,
                   const std::string& db,
                   const BSONObj& commandToRun) {
    uassert(9670414, "Conn is not still connected", conn->isStillConnected());
    auto opMsgQueryBuilder = OpMsgRequestBuilder{};
    auto opMsgQuery = opMsgQueryBuilder.create(
        boost::none, DatabaseName{}.createDatabaseName_forTest(boost::none, db), commandToRun);
    auto [reply, clientBase] = conn->runCommandWithTarget(opMsgQuery);
    return reply->getCommandReply().getOwned();
}

void runCommandAssertOK(DBClientConnection* const conn,
                        const BSONObj& command,
                        const std::string& db,
                        const std::vector<ErrorCodes::Error> acceptableErrorCodes) {
    auto cmdResponse = runCommand(conn, db, command);
    if (cmdResponse.getField("ok").trueValue()) {
        return;
    }
    for (const auto& error : acceptableErrorCodes) {
        if (error == cmdResponse.getField("code").safeNumberInt()) {
            return;
        }
    }
    uasserted(9670420,
              str::stream{} << "Expected OK command result from " << command << " but got "
                            << cmdResponse);
}
}  // namespace mongo::query_tester
