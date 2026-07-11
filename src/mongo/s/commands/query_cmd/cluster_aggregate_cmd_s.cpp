// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/commands/query_cmd/cluster_aggregate_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster aggregate command on mongos.
 */
struct ClusterAggregateCommandS {
    using Request = AggregateCommandRequest;
    static constexpr std::string_view kCommandName = "aggregate"sv;

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
};
MONGO_REGISTER_COMMAND(ClusterAggregateCommandBase<ClusterAggregateCommandS>).forRouter();

}  // namespace
}  // namespace mongo
