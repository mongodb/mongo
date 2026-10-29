// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fle2_get_count_info_command_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <string_view>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

/**
 * Retrieve a set of tags from ESC. Returns a count suitable for either insert or query.
 *
 * Always routes to owning/primary shard for a database because ESC is pinned to a single/primary
 * shard and ESC is not sharded.
 */
class ClusterGetQueryableEncryptionCountInfoCmd final
    : public TypedCommand<ClusterGetQueryableEncryptionCountInfoCmd> {
public:
    using Request = GetQueryableEncryptionCountInfo;
    using Reply = GetQueryableEncryptionCountInfo::Reply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    std::set<std::string_view> sensitiveFieldNames() const final {
        return {GetQueryableEncryptionCountInfo::kTokensFieldName};
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to read tags",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                        ActionType::internal));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };
};
MONGO_REGISTER_COMMAND(ClusterGetQueryableEncryptionCountInfoCmd).forRouter();

ClusterGetQueryableEncryptionCountInfoCmd::Reply
ClusterGetQueryableEncryptionCountInfoCmd::Invocation::typedRun(OperationContext* opCtx) {

    {
        std::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    auto nss = request().getNamespace();

    sharding::router::CollectionRouter router(opCtx, nss);
    return router.routeWithRoutingContext(
        Request::kCommandName, [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            tassert(7924701,
                    "ESC collection cannot be sharded",
                    !routingCtx.getCollectionRoutingInfo(nss).isSharded());

            auto& cmd = request();
            setReadWriteConcern(opCtx, cmd, this);

            auto response = uassertStatusOK(
                executeCommandAgainstShardWithMinKeyChunk(
                    opCtx,
                    routingCtx,
                    nss,
                    CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kIdempotent)
                    .swResponse);

            auto reply = CommandHelpers::filterCommandReplyForPassthrough(response.data);
            uassertStatusOK(getStatusFromCommandResult(reply));
            return Reply::parse(reply.removeField("ok"sv), IDLParserContext{Request::kCommandName});
        });
}

}  // namespace
}  // namespace mongo
