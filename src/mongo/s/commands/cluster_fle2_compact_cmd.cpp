// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fle2_compact_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
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

class ClusterCompactStructuredEncryptionDataCmd final
    : public TypedCommand<ClusterCompactStructuredEncryptionDataCmd> {
public:
    using Request = CompactStructuredEncryptionData;
    using Reply = CompactStructuredEncryptionData::Reply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext* opCtx);

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Not authorized to compact structured encryption data",
                    as->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(request().getNamespace()),
                        ActionType::compactStructuredEncryptionData));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
        return false;
    }

    std::set<std::string_view> sensitiveFieldNames() const final {
        return {CompactStructuredEncryptionData::kCompactionTokensFieldName};
    }
};
MONGO_REGISTER_COMMAND(ClusterCompactStructuredEncryptionDataCmd).forRouter();

using Cmd = ClusterCompactStructuredEncryptionDataCmd;
Cmd::Reply Cmd::Invocation::typedRun(OperationContext* opCtx) {
    {
        std::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    auto nss = request().getNamespace();

    auto req = request();
    generic_argument_util::setMajorityWriteConcern(req, &opCtx->getWriteConcern());

    sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
    return router.route(
        Request::kCommandName, [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
            // Rewrite command verb to _shardSvrCompactStructuredEnccryptionData.
            auto cmd = req.toBSON();
            BSONObjBuilder reqBuilder;
            for (const auto& elem : cmd) {
                if (elem.fieldNameStringData() == Request::kCommandName) {
                    reqBuilder.appendAs(elem, "_shardsvrCompactStructuredEncryptionData");
                } else {
                    reqBuilder.append(elem);
                }
            }

            auto response = uassertStatusOK(
                executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                    opCtx,
                    nss.dbName(),
                    dbInfo,
                    CommandHelpers::filterCommandRequestForPassthrough(reqBuilder.obj()),
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
