/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/commands.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

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
                    str::stream() << "Not authorized to drop collection '" << ns << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns, ActionType::dropCollection));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto nss = request().getNamespace();
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop collection in 'config' database in sharded cluster",
                    nss.db() != NamespaceString::kConfigDb);

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot drop collection in 'admin' database in sharded cluster",
                    nss.db() != NamespaceString::kAdminDb);

            try {
                // Invalidate the routing table cache entry for this collection so that we reload it
                // the next time it is accessed, even if sending the command to the config server
                // fails due to e.g. a NetworkError.
                ON_BLOCK_EXIT([opCtx, nss] {
                    Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
                });

                const auto dbInfo =
                    uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));

                // Send it to the primary shard
                ShardsvrDropCollection dropCollectionCommand(nss);
                dropCollectionCommand.setDbName(nss.db());
                dropCollectionCommand.setCollectionUUID(request().getCollectionUUID());

                auto cmdResponse = executeCommandAgainstDatabasePrimary(
                    opCtx,
                    nss.db(),
                    dbInfo,
                    CommandHelpers::appendMajorityWriteConcern(dropCollectionCommand.toBSON({}),
                                                               opCtx->getWriteConcern()),
                    ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                    Shard::RetryPolicy::kIdempotent);

                const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
                BSONObjBuilder result;
                CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &result);
                auto resultObj = result.obj();
                uassertStatusOK(getStatusFromCommandResult(resultObj));
                // Ensure our reply conforms to the IDL-defined reply structure.
                return DropReply::parse({"drop"}, resultObj);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                uassert(CollectionUUIDMismatchInfo(request().getDbName().toString(),
                                                   *request().getCollectionUUID(),
                                                   request().getNamespace().coll().toString(),
                                                   boost::none),
                        "Database does not exist",
                        !request().getCollectionUUID());

                // If the namespace isn't found, treat the drop as a success but inform about the
                // failure.
                DropReply reply;
                reply.setInfo("database does not exist"_sd);
                return reply;
            }
        }
    };
} clusterDropCmd;

}  // namespace
}  // namespace mongo
