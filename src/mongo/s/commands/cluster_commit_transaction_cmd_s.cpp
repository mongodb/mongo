// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/cluster_commit_transaction_cmd.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster commitTransaction command on mongos.
 */
struct ClusterCommitTransactionCmdS {
    static constexpr std::string_view kName = "commitTransaction"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static Status checkAuthForOperation(OperationContext* opCtx,
                                        const DatabaseName&,
                                        const BSONObj&) {
        return Status::OK();
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterCommitTransactionCmdBase<ClusterCommitTransactionCmdS>).forRouter();

}  // namespace
}  // namespace mongo
