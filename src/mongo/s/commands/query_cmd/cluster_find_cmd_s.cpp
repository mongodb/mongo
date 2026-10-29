// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/s/commands/query_cmd/cluster_find_cmd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster find command on mongos.
 */
struct ClusterFindCmdS {
    using Request = FindCommandRequest;
    static constexpr std::string_view kCommandName = "find"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     bool hasTerm,
                                     const NamespaceString& nss) {
        uassertStatusOK(
            auth::checkAuthForFind(AuthorizationSession::get(opCtx->getClient()), nss, hasTerm));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }

    static void checkCanExplainHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterFindCmdBase<ClusterFindCmdS>).forRouter();

}  // namespace
}  // namespace mongo
