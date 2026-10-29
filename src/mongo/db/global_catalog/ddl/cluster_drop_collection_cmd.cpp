// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/drop_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class DropCmd : public DropCmdVersion1Gen<DropCmd> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto ns = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop collection '"
                                  << ns.toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns, ActionType::dropCollection));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto nss = request().getNamespace();
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop collection in 'config' database in sharded cluster",
                    nss.dbName() != DatabaseName::kConfig);

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop collection in 'admin' database in sharded cluster",
                    nss.dbName() != DatabaseName::kAdmin);

            try {
                // Invalidate the routing table cache entry for this collection so that we reload it
                // the next time it is accessed, even if sending the command to the config server
                // fails due to e.g. a NetworkError.
                ON_BLOCK_EXIT([opCtx, nss] {
                    Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
                });

                // Send it to the primary shard
                ShardsvrDropCollection dropCollectionCommand(nss);
                dropCollectionCommand.setDbName(nss.dbName());
                dropCollectionCommand.setCollectionUUID(request().getCollectionUUID());
                generic_argument_util::setMajorityWriteConcern(dropCollectionCommand,
                                                               &opCtx->getWriteConcern());

                sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
                return router.route(
                    Request::kCommandName,
                    [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                        auto cmdResponse =
                            executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                opCtx,
                                nss.dbName(),
                                dbInfo,
                                dropCollectionCommand.toBSON(),
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                Shard::RetryPolicy::kIdempotent);

                        const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                        auto resultObj =
                            CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data);
                        uassertStatusOK(getStatusFromWriteCommandReply(resultObj));

                        // Ensure our reply conforms to the IDL-defined reply structure.
                        return DropReply::parse(resultObj, IDLParserContext{"drop"});
                    });
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                uassert(CollectionUUIDMismatchInfo(request().getDbName(),
                                                   *request().getCollectionUUID(),
                                                   std::string{request().getNamespace().coll()},
                                                   boost::none),
                        "Database does not exist",
                        !request().getCollectionUUID());

                // If the namespace isn't found, treat the drop as a success but inform about the
                // failure.
                DropReply reply;
                reply.setInfo("database does not exist"sv);
                return reply;
            }
        }
    };
};
MONGO_REGISTER_COMMAND(DropCmd).forRouter();

}  // namespace
}  // namespace mongo
