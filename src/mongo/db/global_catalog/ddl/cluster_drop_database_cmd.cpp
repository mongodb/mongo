// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/drop_database_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class DropDatabaseCmd : public DropDatabaseCmdVersion1Gen<DropDatabaseCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop database '"
                                  << request().getDbName().toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns(), ActionType::dropDatabase));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto dbName = request().getDbName();

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop the config database",
                    dbName != DatabaseName::kConfig);
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop the admin database",
                    dbName != DatabaseName::kAdmin);
            uassert(ErrorCodes::BadValue,
                    "Must pass 1 as the 'dropDatabase' parameter",
                    request().getCommandParameter() == 1);

            try {
                // Invalidate the database metadata so the next access kicks off a full reload, even
                // if sending the command to the config server fails due to e.g. a NetworkError.
                ON_BLOCK_EXIT(
                    [opCtx, dbName] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName); });

                // Send it to the primary shard
                ShardsvrDropDatabase dropDatabaseCommand;
                dropDatabaseCommand.setDbName(dbName);
                generic_argument_util::setMajorityWriteConcern(dropDatabaseCommand,
                                                               &opCtx->getWriteConcern());

                sharding::router::DBPrimaryRouter router(opCtx, dbName);
                router.route(Request::kCommandParameterFieldName,
                             [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                                 auto cmdResponse =
                                     executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                         opCtx,
                                         dbName,
                                         dbInfo,
                                         dropDatabaseCommand.toBSON(),
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent);

                                 const auto remoteResponse =
                                     uassertStatusOK(cmdResponse.swResponse);
                                 uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                             });
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // If the namespace isn't found, treat the drop as a success
            }
            return {};
        }
    };
};
MONGO_REGISTER_COMMAND(DropDatabaseCmd).forRouter();

}  // namespace
}  // namespace mongo
