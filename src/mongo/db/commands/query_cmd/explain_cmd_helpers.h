/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <memory>
namespace mongo {
namespace explain_cmd_helpers {

struct ExplainedCommand {
    std::unique_ptr<OpMsgRequest> innerRequest;
    std::unique_ptr<CommandInvocation> innerInvocation;
};

/**
 * Builds the command to be explained and returns its request and invocation.
 *
 * Throws errors if:
 *  - The given command cannot be found in the 'CommandRegistry'.
 *  - There is a mismatch in db name between the top-level 'explain' and the command that is being
 *    explain-ed.
 *  - 'rawData' is specified but the command does not support 'rawData'.
 */
ExplainedCommand makeExplainedCommand(OperationContext* opCtx,
                                      const OpMsgRequest& opMsgRequest,
                                      const DatabaseName& dbName,
                                      const BSONObj& explainedObj,
                                      ExplainOptions::Verbosity verbosity,
                                      const SerializationContext& serializationContext);

/**
 * Synthesize the BSONObj for the command to be explained on 'mongos'.
 *
 *
 * We need to merge generic arguments of the explain command (e.g. readConcern, maxTimeMS,
 * rawData) into the inner command that is being explained so that when we forward the inner
 * command to shards, we are able to preserve the generic arguments.
 */
BSONObj makeExplainedObjForMongos(const BSONObj& outerObj, const BSONObj& innerObj);

}  // namespace explain_cmd_helpers
}  // namespace mongo
