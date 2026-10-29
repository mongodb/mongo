// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/commands/query_cmd/cluster_getmore_cmd.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <string>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Implements the cluster getMore command on mongos.
 */
struct ClusterGetMoreCmdS {
    using Request = GetMoreCommandRequest;
    using Reply = GetMoreCommandRequest::Reply;
    static constexpr std::string_view kCommandName = "getMore"sv;

    static const std::set<std::string>& getApiVersions() {
        return kApiVersions1;
    }

    static void doCheckAuthorization(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     long long cursorID,
                                     bool hasTerm) {
        uassertStatusOK(auth::checkAuthForGetMore(
            AuthorizationSession::get(opCtx->getClient()), nss, cursorID, hasTerm));
    }

    static void checkCanRunHere(OperationContext* opCtx) {
        // Can always run on a mongos.
    }
};
MONGO_REGISTER_COMMAND(ClusterGetMoreCmdBase<ClusterGetMoreCmdS>).forRouter();

}  // namespace
}  // namespace mongo
