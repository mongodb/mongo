
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

#include "mongo/db/transaction_validation.h"

#include "mongo/db/commands.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

namespace {

// The command names for which to check out a session. These are commands that support retryable
// writes, readConcern snapshot, or multi-statement transactions. We additionally check out the
// session for commands that can take a lock and then run another whitelisted command in
// DBDirectClient. Otherwise, the nested command would try to check out a session under a lock,
// which is not allowed.
const StringMap<int> sessionCheckOutList = {{"abortTransaction", 1},
                                            {"aggregate", 1},
                                            {"applyOps", 1},
                                            {"commitTransaction", 1},
                                            {"count", 1},
                                            {"dbHash", 1},
                                            {"delete", 1},
                                            {"distinct", 1},
                                            {"doTxn", 1},
                                            {"explain", 1},
                                            {"filemd5", 1},
                                            {"find", 1},
                                            {"findandmodify", 1},
                                            {"findAndModify", 1},
                                            {"geoNear", 1},
                                            {"geoSearch", 1},
                                            {"getMore", 1},
                                            {"group", 1},
                                            {"insert", 1},
                                            {"killCursors", 1},
                                            {"prepareTransaction", 1},
                                            {"refreshLogicalSessionCacheNow", 1},
                                            {"update", 1}};

// Commands that can be sent with session info but should not check out a session.
const StringMap<int> skipSessionCheckoutList = {
    {"coordinateCommitTransaction", 1}, {"voteAbortTransaction", 1}, {"voteCommitTransaction", 1}};

bool commandCanCheckOutSession(StringData cmdName) {
    return sessionCheckOutList.find(cmdName) != sessionCheckOutList.cend();
}

}  // namespace

void validateWriteConcernForTransaction(const WriteConcernOptions& wcResult, StringData cmdName) {
    uassert(ErrorCodes::InvalidOptions,
            "writeConcern is not allowed within a multi-statement transaction",
            wcResult.usedDefault || cmdName == "commitTransaction" ||
                cmdName == "coordinateCommitTransaction" || cmdName == "abortTransaction" ||
                cmdName == "prepareTransaction" || cmdName == "doTxn");
}

bool shouldCommandSkipSessionCheckout(StringData cmdName) {
    return skipSessionCheckoutList.find(cmdName) != skipSessionCheckoutList.cend();
}

void validateSessionOptions(const OperationSessionInfoFromClient& sessionOptions,
                            StringData cmdName,
                            StringData dbname) {
    if (sessionOptions.getAutocommit()) {
        uassertStatusOK(CommandHelpers::canUseTransactions(dbname, cmdName));
    }

    if (sessionOptions.getTxnNumber()) {
        uassert(50768,
                str::stream() << "It is illegal to provide a txnNumber for command " << cmdName,
                commandCanCheckOutSession(cmdName) || shouldCommandSkipSessionCheckout(cmdName));
    }

    if (sessionOptions.getStartTransaction()) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                "Cannot run killCursors as the first operation in a multi-document transaction.",
                cmdName != "killCursors");
    }
}

}  // namespace mongo
