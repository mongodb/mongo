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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/ddl/drop_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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


                sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
                return router.route(
                    opCtx,
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
                reply.setInfo("database does not exist"_sd);
                return reply;
            }
        }
    };
};
MONGO_REGISTER_COMMAND(DropCmd).forRouter();

}  // namespace
}  // namespace mongo
