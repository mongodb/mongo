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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/split_chunk_request_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using std::string;

/**
 * Internal sharding command run on config servers to split a chunk.
 *
 * Format:
 * {
 *   _configsvrCommitChunkSplit: <string namespace>,
 *   collEpoch: <OID epoch>,
 *   min: <BSONObj chunkToSplitMin>,
 *   max: <BSONObj chunkToSplitMax>,
 *   splitPoints: [<BSONObj key>, ...],
 *   shard: <string shard>,
 *   writeConcern: <BSONObj>
 * }
 */

constexpr std::string_view kCollectionVersionField = "collectionVersion"sv;

// TODO (SERVER-127253) Remove this command once v9.0 branches out
class ConfigSvrSplitChunkCommand : public BasicCommand {
public:
    ConfigSvrSplitChunkCommand() : BasicCommand("_configsvrCommitChunkSplit") {}

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is sent by a shard to the sharding config server. Do "
               "not call directly. Receives, validates, and processes a SplitChunkRequest.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrCommitChunkSplit can only be run on config servers",
                serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

        auto parsedRequest = uassertStatusOK(SplitChunkRequest::parseFromConfigCommand(cmdObj));

        // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
        // collections under the exclusive _kChunkOpLock happen on the same term.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        const bool isAuthoritative =
            sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
            AuthoritativeMetadataAccessLevelEnum::kNone;

        ShardingCatalogManager::ShardAndCollectionPlacementVersions shardAndCollVers;
        if (!isAuthoritative) {
            // Legacy non-authoritative path: keep the pre-existing behavior. No ACR and no
            // dummy write.
            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            shardAndCollVers = uassertStatusOK(
                ShardingCatalogManager::get(opCtx)->commitChunkSplit(opCtx,
                                                                     parsedRequest.getNamespace(),
                                                                     parsedRequest.getEpoch(),
                                                                     parsedRequest.getTimestamp(),
                                                                     parsedRequest.getChunkRange(),
                                                                     parsedRequest.getSplitPoints(),
                                                                     parsedRequest.getShardName()));
        } else {
            // Authoritative path. The originating chunk-op coordinator on the data-bearing shard
            // attaches a session id and is responsible for installing the new chunk layout into the
            // local shard catalog after this command returns. Use ACR so the session id stays held
            // while the catalog updates run; the trailing dummy write below bumps the txnNumber on
            // the oplog so that an older request on the same session cannot replay onto a newer
            // state.
            {
                auto newClient =
                    opCtx->getServiceContext()->getService()->makeClient("CommitChunkSplit");
                AlternativeClientRegion acr(newClient);
                auto executor =
                    Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
                auto newOpCtxPtr = CancelableOperationContext(
                    cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

                AuthorizationSession::get(newOpCtxPtr.get()->getClient())
                    ->grantInternalAuthorization();
                newOpCtxPtr->setWriteConcern(opCtx->getWriteConcern());
                repl::ReadConcernArgs::get(newOpCtxPtr.get()) =
                    repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

                shardAndCollVers =
                    uassertStatusOK(ShardingCatalogManager::get(newOpCtxPtr.get())
                                        ->commitChunkSplit(newOpCtxPtr.get(),
                                                           parsedRequest.getNamespace(),
                                                           parsedRequest.getEpoch(),
                                                           parsedRequest.getTimestamp(),
                                                           parsedRequest.getChunkRange(),
                                                           parsedRequest.getSplitPoints(),
                                                           parsedRequest.getShardName()));
            }

            // No write happened on this txnNumber in the parent opCtx, so make a dummy write to
            // protect against older requests with old txnNumbers being replayed.
            DBDirectClient client(opCtx);
            client.update(NamespaceString::kServerConfigurationNamespace,
                          BSON("_id" << "commitChunkSplitStats"),
                          BSON("$inc" << BSON("count" << 1)),
                          true /* upsert */,
                          false /* multi */);
        }

        shardAndCollVers.collectionPlacementVersion.serialize(kCollectionVersionField, &result);
        shardAndCollVers.shardPlacementVersion.serialize(ChunkVersion::kChunkVersionField, &result);

        return true;
    }
};
MONGO_REGISTER_COMMAND(ConfigSvrSplitChunkCommand).forShard();
}  // namespace
}  // namespace mongo
