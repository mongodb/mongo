// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/write_commands_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/s/commands/query_cmd/cluster_write_cmd.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

struct ClusterInsertCmdS {
    static constexpr std::string_view kName = "insert"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::InsertCommandRequest& op) {
        auth::checkAuthForInsertCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterInsertCmdBase<ClusterInsertCmdS>).forRouter();

struct ClusterUpdateCmdS {
    static constexpr std::string_view kName = "update"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::UpdateCommandRequest& op) {
        auth::checkAuthForUpdateCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterUpdateCmdBase<ClusterUpdateCmdS>).forRouter();

struct ClusterDeleteCmdS {
    static constexpr std::string_view kName = "delete"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(AuthorizationSession* authzSession,
                                     bool bypass,
                                     const write_ops::DeleteCommandRequest& op) {
        auth::checkAuthForDeleteCommand(authzSession, bypass, op);
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterDeleteCmdBase<ClusterDeleteCmdS>).forRouter();

}  // namespace
}  // namespace mongo
