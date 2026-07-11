// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/query_cmd/cluster_bulk_write_cmd.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

struct ClusterBulkWriteCmdS {
    static constexpr std::string_view kName = "bulkWrite"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const BulkWriteCommandRequest& op) {
        uassert(ErrorCodes::Unauthorized,
                "unauthorized",
                authzSession->isAuthorizedForPrivileges(bulk_write_common::getPrivileges(op)));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};

MONGO_REGISTER_COMMAND(ClusterBulkWriteCmd<ClusterBulkWriteCmdS>).forRouter();

}  // namespace
}  // namespace mongo
