// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_tester/command_helpers.h"

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
