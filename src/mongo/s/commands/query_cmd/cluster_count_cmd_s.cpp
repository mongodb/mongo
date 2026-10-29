// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/query_cmd/cluster_count_cmd.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {

/**
 * Implements the cluster count command on mongos.
 */
struct ClusterCountCmdS {
    using Request = CountCommandRequest;
    using Reply = CountCommandRequest::Reply;
    static constexpr std::string_view kCommandName = Request::kCommandName;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static Status checkAuthForOperation(OperationContext*, const DatabaseName&, const Request&) {
        // No additional required privileges on a mongos.
        return Status::OK();
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterCountCmdBase<ClusterCountCmdS>).forRouter();

}  // namespace
}  // namespace mongo
