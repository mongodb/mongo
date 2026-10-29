// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

#include <boost/optional/optional.hpp>

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

/**
 * Resolves the effective maxTimeMS for an explain command from the explain command's own maxTimeMS
 * and a maxTimeMS nested inside the explained command. When both are positive the smaller (more
 * restrictive) value wins. A value of 0 means "no limit" and never overrides a positive value; an
 * explicit 0 in either placement is preserved (rather than collapsing to "unset") so it bypasses
 * defaultMaxTimeMS. The result is unset only when both placements are unset. Folding the result
 * back into the explain command's maxTimeMS lets the standard deadline machinery enforce a nested
 * maxTimeMS exactly like a top-level one.
 */
boost::optional<std::int64_t> resolveMaxTimeMS(boost::optional<std::int64_t> explainMaxTimeMS,
                                               boost::optional<std::int64_t> nestedMaxTimeMS);

}  // namespace explain_cmd_helpers
}  // namespace mongo
