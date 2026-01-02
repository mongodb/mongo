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

#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <typeinfo>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

// Writes to the local shard. It shall only be used to write to collections that are always
// untracked (e.g. collections under the admin/config db).
void writeToLocalShard(OperationContext* opCtx,
                       const BatchedCommandRequest& batchedCommandRequest,
                       const WriteConcernOptions& writeConcern) {
    tassert(8144401,
            "Forbidden to write directly to local shard unless namespace is always shard local",
            batchedCommandRequest.getNS().isShardLocalNamespace());

    // A request dispatched through a local client is served within the same thread that submits it
    // (so that the opCtx needs to be used as the vehicle to pass the WC to the ServiceEntryPoint).
    const auto originalWC = opCtx->getWriteConcern();
    ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
    opCtx->setWriteConcern(writeConcern);

    const BSONObj cmdObj = [&] {
        BSONObjBuilder cmdObjBuilder;
        batchedCommandRequest.serialize(&cmdObjBuilder);
        cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField, writeConcern.toBSON());
        return cmdObjBuilder.obj();
    }();


    auto shState = ShardingState::get(opCtx);
    invariant(shState->enabled());
    auto shardId = shState->shardId();
    auto shardState = ShardSharedStateCache::get(opCtx).getShardState(shardId);

    Shard::RetryStrategy retryStrategy{ConnectionString::ConnectionType::kLocal,
                                       *shardState,
                                       Shard::RetryPolicy::kStrictlyNotIdempotent};

    uassertStatusOK(runWithRetryStrategy(
        opCtx, retryStrategy, [&](const TargetingMetadata&) -> RetryStrategy::Result<BSONObj> {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            const auto primaryHostAndPort = replCoord->getCurrentPrimaryHostAndPort();
            const auto cmdResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                opCtx,
                batchedCommandRequest.getNS().dbName(),
                cmdObj,
                [](executor::TaskExecutor::CallbackHandle handle) {},
                [](executor::TaskExecutor::CallbackHandle handle) {});

            const auto status = getStatusFromCommandResult(cmdResponse);

            if (!status.isOK()) {
                return {status, executor::extractErrorLabels(cmdResponse), primaryHostAndPort};
            }

            return RetryStrategy::Result{cmdResponse, primaryHostAndPort};
        }));
}

}  // namespace

bool ShardServerProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    // The RoutingContext is acquired and disposed of without validating the routing tables against
    // a shard here because this isSharded() check is only used for distributed query planning
    // optimizations; it doesn't affect query correctness.
    auto routingCtx = uassertStatusOK(getRoutingContext(opCtx, {nss}));
    return routingCtx->getCollectionRoutingInfo(nss).isSharded();
}

void ShardServerProcessInterface::checkRoutingInfoEpochOrThrow(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    ChunkVersion targetCollectionPlacementVersion) const {
    auto* catalogCache = Grid::get(expCtx->getOperationContext())->catalogCache();

    auto receivedVersion = [&] {
        // Mark the cache entry routingInfo for the 'nss' if the entry is staler than
        // 'targetCollectionPlacementVersion'.
        auto ignoreIndexVersion = ShardVersionFactory::make(targetCollectionPlacementVersion);

        catalogCache->onStaleCollectionVersion(nss, ignoreIndexVersion);
        return ignoreIndexVersion;
    }();

    auto wantedVersion = [&] {
        // TODO SERVER-95749 Avoid forced collection cache refresh and validate RoutingContext,
        // throwing if stale.
        auto routingInfo = uassertStatusOK(
            catalogCache->getCollectionRoutingInfo(expCtx->getOperationContext(), nss));
        auto foundVersion = routingInfo.hasRoutingTable()
            ? routingInfo.getCollectionVersion().placementVersion()
            : ChunkVersion::UNTRACKED();

        auto ignoreIndexVersion = ShardVersionFactory::make(foundVersion);
        return ignoreIndexVersion;
    }();

    uassert(StaleEpochInfo(nss, receivedVersion, wantedVersion),
            str::stream() << "Could not act as router for " << nss.toStringForErrorMsg()
                          << ", received " << receivedVersion.toString() << ", but found "
                          << wantedVersion.toString(),
            wantedVersion.placementVersion().isSameCollection(receivedVersion.placementVersion()));
}

boost::optional<Document> ShardServerProcessInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    // We only want to retrieve the one document that corresponds to 'documentKey', so we
    // ignore collation when computing which shard to target.
    pipeline_factory::MakePipelineOptions opts;
    opts.shardTargetingPolicy = ShardTargetingPolicy::kForceTargetingWithSimpleCollation;
    opts.readConcern = std::move(readConcern);

    // Do not inherit the collator from 'expCtx', but rather use the target collection default
    // collator. This is relevant in case of attaching a cursor for local read.
    opts.useCollectionDefaultCollator = true;

    return doLookupSingleDocument(
        expCtx, nss, std::move(collectionUUID), documentKey, std::move(opts));
}

MongoProcessInterface::InsertResult ShardServerProcessInterface::insert(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
    const WriteConcernOptions& wc,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest batchInsertCommand(std::move(insertCommand));

    const auto originalWC = expCtx->getOperationContext()->getWriteConcern();
    ScopeGuard resetWCGuard([&] { expCtx->getOperationContext()->setWriteConcern(originalWC); });
    expCtx->getOperationContext()->setWriteConcern(wc);

    cluster::write(expCtx->getOperationContext(),
                   batchInsertCommand,
                   nullptr /* nss */,
                   &stats,
                   &response,
                   targetEpoch);

    InsertResult result;
    if (!response.getOk()) {
        result.emplace_back(0, response.getTopLevelStatus());
    } else if (response.isErrDetailsSet()) {
        result.reserve(response.getErrDetails().size());
        result.assign(response.getErrDetails().begin(), response.getErrDetails().end());
    } else if (response.isWriteConcernErrorSet()) {
        result.emplace_back(0, response.getWriteConcernError()->toStatus());
    }
    return result;
}

StatusWith<MongoProcessInterface::UpdateResult> ShardServerProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::UpdateCommandRequest> updateCommand,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest batchUpdateCommand(std::move(updateCommand));

    const auto originalWC = expCtx->getOperationContext()->getWriteConcern();
    ScopeGuard resetWCGuard([&] { expCtx->getOperationContext()->setWriteConcern(originalWC); });
    expCtx->getOperationContext()->setWriteConcern(wc);

    cluster::write(expCtx->getOperationContext(),
                   batchUpdateCommand,
                   nullptr /* nss */,
                   &stats,
                   &response,
                   targetEpoch);

    if (auto status = response.toStatus(); status != Status::OK()) {
        return status;
    }
    return {{response.getN(), response.getNModified()}};
}

BSONObj ShardServerProcessInterface::finalizePipelineAndExplain(
    std::unique_ptr<Pipeline> pipeline,
    ExplainOptions::Verbosity verbosity,
    std::function<void(Pipeline* pipeline)> optimizePipeline) {
    auto firstStage = pipeline->peekFront();
    // We don't want to send an internal stage to the shards.
    if (firstStage &&
        (typeid(*firstStage) == typeid(DocumentSourceMerge) ||
         typeid(*firstStage) == typeid(DocumentSourceMergeCursors) ||
         typeid(*firstStage) == typeid(DocumentSourceCursor))) {
        pipeline->popFront();
    }
    return sharded_agg_helpers::finalizePipelineAndTargetShardsForExplain(std::move(pipeline),
                                                                          optimizePipeline);
}

void ShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const NamespaceString& sourceNs,
    const NamespaceString& targetNs,
    bool dropTarget,
    bool stayTemp,
    const BSONObj& originalCollectionOptions,
    const std::vector<BSONObj>& originalIndexes) {
    sharding::router::DBPrimaryRouter router(opCtx, sourceNs.dbName());
    router.route("ShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged",
                 [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                     auto newCmdObj = CommonMongodProcessInterface::_convertRenameToInternalRename(
                         opCtx, sourceNs, targetNs, originalCollectionOptions, originalIndexes);
                     BSONObjBuilder newCmdWithWriteConcernBuilder(std::move(newCmdObj));
                     newCmdWithWriteConcernBuilder.append(WriteConcernOptions::kWriteConcernField,
                                                          opCtx->getWriteConcern().toBSON());
                     newCmdObj = newCmdWithWriteConcernBuilder.done();
                     auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                         opCtx,
                         // internalRenameIfOptionsAndIndexesMatch is adminOnly.
                         DatabaseName::kAdmin,
                         cdb,
                         newCmdObj,
                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                         Shard::RetryPolicy::kStrictlyNotIdempotent);
                     uassertStatusOKWithContext(response.swResponse,
                                                str::stream() << "failed while running command "
                                                              << newCmdObj);
                     auto result = response.swResponse.getValue().data;
                     uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                                str::stream() << "failed while running command "
                                                              << newCmdObj);
                     uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                                                str::stream() << "failed while running command "
                                                              << newCmdObj);
                 });
}

BSONObj ShardServerProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    return _getCollectionOptions(opCtx, nss);
}

BSONObj ShardServerProcessInterface::_getCollectionOptions(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           bool runOnPrimary) {
    if (nss.isNamespaceAlwaysUntracked()) {
        return getCollectionOptionsLocally(opCtx, nss);
    };

    const auto response = _runListCollectionsCommandOnAShardedCluster(
        opCtx,
        nss,

        {// For viewless timeseries collections, fetch the raw collection options.
         // This is consistent with the other implementations of this method, which fetch the
         // collection options from the catalog without undergoing any timeseries translation.
         // (Note that for all other collection types, this parameter has no effect.)
         .rawData = gFeatureFlagAllBinariesSupportRawDataOperations.isEnabled(
             VersionContext::getDecoration(opCtx),
             serverGlobalParams.featureCompatibility.acquireFCVSnapshot()),
         // Some collections (for example temp collections) only exist on the replica set primary so
         // we may need to run on the primary to get the options.
         .runOnPrimary = runOnPrimary});
    if (response.empty()) {
        return BSONObj{};
    }

    BSONObj listCollectionsResult = response[0].getOwned();
    const BSONElement optionsElement = listCollectionsResult["options"];
    if (optionsElement) {
        auto optionObj = optionsElement.Obj();

        // If the BSON object has field 'info' and the BSON element 'info' has field
        // 'uuid', then extract the uuid and add to the BSON object to be returned.
        // This will ensure that the BSON object is complaint with the BSON object
        // returned for non-sharded namespace.
        if (auto infoElement = listCollectionsResult["info"]; infoElement && infoElement["uuid"]) {
            return optionObj.addField(infoElement["uuid"]);
        }
        return optionObj.getOwned();
    }
    return BSONObj{};
}

UUID ShardServerProcessInterface::fetchCollectionUUIDFromPrimary(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
    const auto options = _getCollectionOptions(opCtx, nss, /*runOnPrimary*/ true);
    auto uuid = UUID::parse(options["uuid"_sd]);
    return uassertStatusOK(uuid);
}

query_shape::CollectionType ShardServerProcessInterface::getCollectionType(
    OperationContext* opCtx, const NamespaceString& nss) {

    if (nss.isNamespaceAlwaysUntracked()) {
        return getCollectionTypeLocally(opCtx, nss);
    };

    const auto response = _runListCollectionsCommandOnAShardedCluster(opCtx, nss, {});
    if (response.empty()) {
        return query_shape::CollectionType::kNonExistent;
    }
    const BSONObj& listCollectionsResult = response[0];

    const StringData typeString = listCollectionsResult["type"].valueStringDataSafe();
    tassert(9072002,
            "All collections returned by listCollections must have a type element",
            !typeString.empty());

    if (typeString == "collection") {
        return query_shape::CollectionType::kCollection;
    }
    if (typeString == "timeseries") {
        return query_shape::CollectionType::kTimeseries;
    }
    if (typeString == "view") {
        return query_shape::CollectionType::kView;
    }
    MONGO_UNREACHABLE_TASSERT(9072003);
}

std::vector<BSONObj> ShardServerProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                bool includeBuildUUIDs) {
    sharding::router::CollectionRouter router(opCtx, ns);
    return router.routeWithRoutingContext(
        "ShardServerProcessInterface::getIndexSpecs",
        [&](OperationContext* opCtx, RoutingContext& routingCtx) -> std::vector<BSONObj> {
            StatusWith<Shard::QueryResponse> response =
                loadIndexesFromAuthoritativeShard(opCtx, routingCtx, ns);
            if (response.getStatus().code() == ErrorCodes::NamespaceNotFound) {
                return {};
            }
            uassertStatusOK(response);
            return response.getValue().docs;
        });
}

std::vector<DatabaseName> ShardServerProcessInterface::getAllDatabases(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    return _getAllDatabasesOnAShardedCluster(opCtx, tenantId);
}

std::vector<BSONObj> ShardServerProcessInterface::runListCollections(OperationContext* opCtx,
                                                                     const DatabaseName& db,
                                                                     bool addPrimaryShard) {
    return _runListCollectionsCommandOnAShardedCluster(
        opCtx, NamespaceStringUtil::deserialize(db, ""), {.addPrimaryShard = addPrimaryShard});
}

void ShardServerProcessInterface::_createCollectionCommon(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          const BSONObj& cmdObj,
                                                          boost::optional<ShardId> dataShard) {
    // TODO (SERVER-77915): Remove the FCV check and keep only the 'else' branch
    if (!feature_flags::g80CollectionCreationPath.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        sharding::router::DBPrimaryRouter router(opCtx, dbName);
        router.createDbImplicitlyOnRoute(dataShard);
        router.route("ShardServerProcessInterface::_createCollectionCommon",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         BSONObjBuilder finalCmdBuilder(cmdObj);
                         finalCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                                                opCtx->getWriteConcern().toBSON());
                         BSONObj finalCmdObj = finalCmdBuilder.obj();
                         auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                             opCtx,
                             dbName,
                             cdb,
                             finalCmdObj,
                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                             Shard::RetryPolicy::kIdempotent);
                         uassertStatusOKWithContext(response.swResponse,
                                                    str::stream() << "failed while running command "
                                                                  << finalCmdObj);
                         auto result = response.swResponse.getValue().data;
                         uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                                    str::stream() << "failed while running command "
                                                                  << finalCmdObj);
                         uassertStatusOKWithContext(
                             getWriteConcernStatusFromCommandResult(result),
                             str::stream()
                                 << "write concern failed while running command " << finalCmdObj);
                     });
    } else {
        const auto collName = cmdObj.firstElement().String();
        const auto nss = NamespaceStringUtil::deserialize(dbName, collName);

        // Creating the ShardsvrCreateCollectionRequest by parsing the {create..} bsonObj guarantees
        // to propagate the apiVersion and apiStrict paramers. Note that shardsvrCreateCollection as
        // internal command will skip the apiVersionCheck. However in case of view, the create
        // command might run an aggregation. Having those fields propagated guarantees the api
        // version check will keep working within the aggregation framework.
        auto request = ShardsvrCreateCollectionRequest::parse(cmdObj, IDLParserContext("create"));

        ShardsvrCreateCollection shardsvrCollCommand(nss);
        request.setUnsplittable(true);

        // Configure the data shard if one was requested.
        request.setDataShard(dataShard);

        shardsvrCollCommand.setShardsvrCreateCollectionRequest(request);
        sharding::router::DBPrimaryRouter router(opCtx, dbName);
        router.createDbImplicitlyOnRoute(dataShard);
        router.route("ShardServerProcessInterface::_createCollectionCommon",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         cluster::createCollection(opCtx, shardsvrCollCommand);
                     });
    }
}

void ShardServerProcessInterface::createCollection(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   const BSONObj& cmdObj) {
    _createCollectionCommon(opCtx, dbName, cmdObj);
}

void ShardServerProcessInterface::createTempCollection(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const BSONObj& collectionOptions,
                                                       boost::optional<ShardId> dataShard) {
    // Insert an entry on the 'kAggTempCollections' collection on this shard to indicate that 'nss'
    // is a temporary collection that shall be garbage-collected (dropped) on the next stepup.
    BatchedCommandRequest bcr(write_ops::InsertCommandRequest{
        NamespaceString(NamespaceString::kAggTempCollections),
        std::vector<BSONObj>({BSON(
            "_id" << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))})});
    writeToLocalShard(opCtx, bcr, defaultMajorityWriteConcernDoNotUse());

    // Create the collection. Note we don't set the 'temp: true' option. The temporary-ness comes
    // from having registered on kAggTempCollections.
    BSONObjBuilder cmd;
    cmd << "create" << nss.coll();
    cmd.appendElementsUnique(collectionOptions);
    _createCollectionCommon(opCtx, nss.dbName(), cmd.done(), std::move(dataShard));
}

void ShardServerProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    sharding::router::CollectionRouter router(opCtx, ns);
    router.routeWithRoutingContext(
        fmt::format("copying index for empty collection {}",
                    NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault())),
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            BSONObjBuilder cmdBuilder;
            cmdBuilder.append("createIndexes", ns.coll());
            cmdBuilder.append("indexes", indexSpecs);
            cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                              opCtx->getWriteConcern().toBSON());
            auto cmdObj = cmdBuilder.obj();
            auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                routingCtx,
                ns,
                cmdObj,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kStrictlyNotIdempotent,
                BSONObj() /*query*/,
                BSONObj() /*collation*/,
                boost::none /*letParameters*/,
                boost::none /*runtimeConstants*/);

            for (const auto& response : shardResponses) {
                uassertStatusOKWithContext(response.swResponse,
                                           str::stream() << "command was not sent " << cmdObj
                                                         << " to shard " << response.shardId);
                const auto& result = response.swResponse.getValue().data;
                uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                           str::stream() << "command was sent but failed " << cmdObj
                                                         << " on shard " << response.shardId);
                uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                                           str::stream()
                                               << "command was sent and succeeded, but failed "
                                                  "waiting for write concern "
                                               << cmdObj << " on shard " << response.shardId);
            }
        });
}

void ShardServerProcessInterface::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    // Build and execute the _shardsvrDropCollection command against the primary shard of the given
    // database.
    sharding::router::DBPrimaryRouter router(opCtx, ns.dbName());
    try {
        router.route("ShardServerProcessInterface::dropCollection",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         ShardsvrDropCollection dropCollectionCommand(ns);
                         generic_argument_util::setMajorityWriteConcern(dropCollectionCommand,
                                                                        &opCtx->getWriteConcern());
                         BSONObj cmdObj = dropCollectionCommand.toBSON();
                         auto response = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                             opCtx,
                             ns.dbName(),
                             cdb,
                             cmdObj,
                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                             Shard::RetryPolicy::kIdempotent);
                         uassertStatusOKWithContext(response.swResponse,
                                                    str::stream() << "failed while running command "
                                                                  << cmdObj);
                         auto result = response.swResponse.getValue().data;
                         uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                                    str::stream() << "failed while running command "
                                                                  << cmdObj);
                         uassertStatusOKWithContext(
                             getWriteConcernStatusFromCommandResult(result),
                             str::stream()
                                 << "write concern failed while running command " << cmdObj);
                     });
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The database might have been dropped by a different operation, so the collection no
        // longer exists.
    }
}

void ShardServerProcessInterface::dropTempCollection(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    // Drop the collection.
    dropCollection(opCtx, nss);

    // Remove the garbage-collector entry associated to it.
    BatchedCommandRequest bcr(write_ops::DeleteCommandRequest{
        NamespaceString(NamespaceString::kAggTempCollections),
        {write_ops::DeleteOpEntry(BSON("_id" << NamespaceStringUtil::serialize(
                                           nss, SerializationContext::stateDefault())),
                                  false /* multi */)}});
    writeToLocalShard(opCtx, std::move(bcr), defaultMajorityWriteConcernDoNotUse());
}

void ShardServerProcessInterface::createTimeseriesView(OperationContext* opCtx,
                                                       const NamespaceString& ns,
                                                       const BSONObj& cmdObj,
                                                       const TimeseriesOptions& userOpts) {
    try {
        ShardServerProcessInterface::createCollection(opCtx, ns.dbName(), cmdObj);
    } catch (const DBException& ex) {
        _handleTimeseriesCreateError(ex, opCtx, ns, userOpts);
    }
}

boost::optional<TimeseriesOptions> ShardServerProcessInterface::_getTimeseriesOptions(
    OperationContext* opCtx, const NamespaceString& ns) {
    const BSONObj options = getCollectionOptions(opCtx, ns);
    if (options.isEmpty()) {
        return boost::none;
    }
    const BSONElement timeseries = options["timeseries"];
    if (!timeseries || !timeseries.isABSONObj()) {
        return boost::none;
    }
    return TimeseriesOptions::parseOwned(timeseries.Obj().getOwned(),
                                         IDLParserContext("TimeseriesOptions"));
}

MongoProcessInterface::InsertResult ShardServerProcessInterface::insertTimeseries(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    std::unique_ptr<write_ops::InsertCommandRequest> insertCommand,
    const WriteConcernOptions& wc,
    boost::optional<OID> targetEpoch) {
    return ShardServerProcessInterface::insert(
        expCtx, ns, std::move(insertCommand), wc, targetEpoch);
}

std::unique_ptr<Pipeline> ShardServerProcessInterface::finalizeAndMaybePreparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<Pipeline> pipeline,
    bool attachCursorAfterOptimizing,
    std::function<void(Pipeline* pipeline)> optimizePipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return sharded_agg_helpers::finalizeAndMaybePreparePipelineForExecution(
        expCtx,
        std::move(pipeline),
        attachCursorAfterOptimizing,
        optimizePipeline,
        shardTargetingPolicy,
        readConcern,
        shouldUseCollectionDefaultCollator);
}

std::unique_ptr<Pipeline> ShardServerProcessInterface::preparePipelineForExecution(
    std::unique_ptr<Pipeline> pipeline,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    return sharded_agg_helpers::preparePipelineForExecution(
        std::move(pipeline), shardTargetingPolicy, std::move(readConcern));
}

std::unique_ptr<Pipeline> ShardServerProcessInterface::preparePipelineForExecution(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggregateCommandRequest& aggRequest,
    std::unique_ptr<Pipeline> pipeline,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern,
    bool shouldUseCollectionDefaultCollator) {
    return sharded_agg_helpers::targetShardsAndAddMergeCursors(
        expCtx,
        std::make_pair(aggRequest, std::move(pipeline)),
        shardCursorsSortSpec,
        shardTargetingPolicy,
        std::move(readConcern),
        shouldUseCollectionDefaultCollator);
}

std::unique_ptr<MongoProcessInterface::ScopedExpectUntrackedCollection>
ShardServerProcessInterface::expectUntrackedCollectionInScope(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<DatabaseVersion>& dbVersion) {
    class ScopedExpectUntrackedCollectionImpl : public ScopedExpectUntrackedCollection {
    public:
        ScopedExpectUntrackedCollectionImpl(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const boost::optional<DatabaseVersion>& dbVersion)
            : _expectUntracked(opCtx, nss, ShardVersion::UNTRACKED(), dbVersion) {}

    private:
        ScopedSetShardRole _expectUntracked;
    };

    return std::make_unique<ScopedExpectUntrackedCollectionImpl>(opCtx, nss, dbVersion);
}

}  // namespace mongo
