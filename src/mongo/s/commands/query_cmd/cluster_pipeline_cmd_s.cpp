/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/commands/query_cmd/cluster_pipeline_cmd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"

namespace mongo {
namespace {

/**
 * Implements the cluster aggregate command on mongos.
 */
struct ClusterPipelineCommandS {
    static constexpr StringData kName = "aggregate"_sd;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     const OpMsgRequest&,
                                     const PrivilegeVector& privileges) {
        uassert(
            ErrorCodes::Unauthorized,
            "unauthorized",
            AuthorizationSession::get(opCtx->getClient())->isAuthorizedForPrivileges(privileges));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static AggregateCommandRequest parseAggregationRequest(
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
        return aggregation_request_helper::parseFromBSON(
            opMsgRequest.body, opMsgRequest.validatedTenancyScope, explainVerbosity);
    }
};
MONGO_REGISTER_COMMAND(ClusterPipelineCommandBase<ClusterPipelineCommandS>).forRouter();

}  // namespace
}  // namespace mongo
