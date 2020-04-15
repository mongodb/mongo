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

#pragma once

#include <atomic>

#include "mongo/client/connection_string.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class DbMessage;
struct DbResponse;
class Message;
class NamespaceString;
class OperationContext;
class QueryRequest;

/**
 * Legacy interface for processing client read/write/cmd requests.
 */
class Strategy {
public:
    /**
     * Handles a legacy-style opQuery request and sends the response back on success or throws on
     * error.
     *
     * Must not be called with legacy '.$cmd' commands.
     */
    static DbResponse queryOp(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm);

    /**
     * Handles a legacy-style getMore request and sends the response back on success (or cursor not
     * found) or throws on error.
     */
    static DbResponse getMore(OperationContext* opCtx, const NamespaceString& nss, DbMessage* dbm);

    /**
     * Handles a legacy-style killCursors request. Doesn't send any response on success or throws on
     * error.
     */
    static void killCursors(OperationContext* opCtx, DbMessage* dbm);

    /**
     * Handles a legacy-style write operation request and updates the last error state on the client
     * with the result from the operation. Doesn't send any response back and does not throw on
     * errors.
     */
    static void writeOp(OperationContext* opCtx, DbMessage* dbm);

    /**
     * Executes a command from either OP_QUERY or OP_MSG wire protocols.
     *
     * Catches StaleConfigException errors and retries the command automatically after refreshing
     * the metadata for the failing namespace.
     */
    static DbResponse clientCommand(OperationContext* opCtx, const Message& message);

    /**
     * Helper to run an explain of a find operation on the shards. Fills 'out' with the result of
     * the of the explain command on success. On failure, throws and does not modify 'out'.
     *
     * Used both if mongos receives an explain command and if it receives an OP_QUERY find with the
     * $explain modifier.
     */
    static void explainFind(OperationContext* opCtx,
                            const BSONObj& findCommand,
                            const QueryRequest& qr,
                            ExplainOptions::Verbosity verbosity,
                            const ReadPreferenceSetting& readPref,
                            BSONObjBuilder* out);
};

}  // namespace mongo
