// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/cluster_abort_transaction_cmd.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster abortTransaction command on mongos.
 */
struct ClusterAbortTransactionCmdS {
    static constexpr std::string_view kName = "abortTransaction"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static Status checkAuthForOperation(OperationContext*, const DatabaseName&, const BSONObj&) {
        return Status::OK();
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterAbortTransactionCmdBase<ClusterAbortTransactionCmdS>).forRouter();

}  // namespace
}  // namespace mongo
