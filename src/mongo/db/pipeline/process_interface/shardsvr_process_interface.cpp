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

#include <fmt/format.h>

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_merge.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/router_role.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using namespace fmt::literals;

bool ShardServerProcessInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    const auto [cm, _] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    return cm.isSharded();
}

void ShardServerProcessInterface::checkRoutingInfoEpochOrThrow(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    ChunkVersion targetCollectionPlacementVersion) const {
    auto const shardId = ShardingState::get(expCtx->opCtx)->shardId();
    auto* catalogCache = Grid::get(expCtx->opCtx)->catalogCache();

    auto receivedVersion = [&] {
        // Since we are only checking the epoch, don't advance the time in store of the index cache
        auto currentShardingIndexCatalogInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(expCtx->opCtx, nss)).sii;

        // Mark the cache entry routingInfo for the 'nss' and 'shardId' if the entry is staler than
        // 'targetCollectionPlacementVersion'.
        auto ignoreIndexVersion = ShardVersionFactory::make(
            targetCollectionPlacementVersion,
            currentShardingIndexCatalogInfo
                ? boost::make_optional(currentShardingIndexCatalogInfo->getCollectionIndexes())
                : boost::none);

        catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
            nss, ignoreIndexVersion, shardId);
        return ignoreIndexVersion;
    }();

    auto wantedVersion = [&] {
        auto routingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(expCtx->opCtx, nss));
        auto foundVersion =
            routingInfo.cm.isSharded() ? routingInfo.cm.getVersion() : ChunkVersion::UNSHARDED();

        auto ignoreIndexVersion = ShardVersionFactory::make(
            foundVersion,
            routingInfo.sii ? boost::make_optional(routingInfo.sii->getCollectionIndexes())
                            : boost::none);
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
    UUID collectionUUID,
    const Document& documentKey,
    boost::optional<BSONObj> readConcern) {
    // We only want to retrieve the one document that corresponds to 'documentKey', so we
    // ignore collation when computing which shard to target.
    MakePipelineOptions opts;
    opts.shardTargetingPolicy = ShardTargetingPolicy::kForceTargetingWithSimpleCollation;
    opts.readConcern = std::move(readConcern);

    return doLookupSingleDocument(expCtx, nss, collectionUUID, documentKey, std::move(opts));
}

Status ShardServerProcessInterface::insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const NamespaceString& ns,
                                           std::vector<BSONObj>&& objs,
                                           const WriteConcernOptions& wc,
                                           boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest insertCommand(
        buildInsertOp(ns, std::move(objs), expCtx->bypassDocumentValidation));

    insertCommand.setWriteConcern(wc.toBSON());

    cluster::write(expCtx->opCtx, insertCommand, &stats, &response, targetEpoch);

    return response.toStatus();
}

StatusWith<MongoProcessInterface::UpdateResult> ShardServerProcessInterface::update(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& ns,
    BatchedObjects&& batch,
    const WriteConcernOptions& wc,
    UpsertType upsert,
    bool multi,
    boost::optional<OID> targetEpoch) {
    BatchedCommandResponse response;
    BatchWriteExecStats stats;

    BatchedCommandRequest updateCommand(buildUpdateOp(expCtx, ns, std::move(batch), upsert, multi));

    updateCommand.setWriteConcern(wc.toBSON());

    cluster::write(expCtx->opCtx, updateCommand, &stats, &response, targetEpoch);

    if (auto status = response.toStatus(); status != Status::OK()) {
        return status;
    }
    return {{response.getN(), response.getNModified()}};
}

BSONObj ShardServerProcessInterface::preparePipelineAndExplain(
    Pipeline* ownedPipeline, ExplainOptions::Verbosity verbosity) {
    auto firstStage = ownedPipeline->peekFront();
    // We don't want to send an internal stage to the shards.
    if (firstStage &&
        (typeid(*firstStage) == typeid(DocumentSourceMerge) ||
         typeid(*firstStage) == typeid(DocumentSourceMergeCursors) ||
         typeid(*firstStage) == typeid(DocumentSourceCursor))) {
        ownedPipeline->popFront();
    }
    return sharded_agg_helpers::targetShardsForExplain(ownedPipeline);
}

void ShardServerProcessInterface::renameIfOptionsAndIndexesHaveNotChanged(
    OperationContext* opCtx,
    const NamespaceString& sourceNs,
    const NamespaceString& targetNs,
    bool dropTarget,
    bool stayTemp,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, targetNs.db()));
    auto newCmdObj = CommonMongodProcessInterface::_convertRenameToInternalRename(
        opCtx, sourceNs, targetNs, originalCollectionOptions, originalIndexes);
    BSONObjBuilder newCmdWithWriteConcernBuilder(std::move(newCmdObj));
    newCmdWithWriteConcernBuilder.append(WriteConcernOptions::kWriteConcernField,
                                         opCtx->getWriteConcern().toBSON());
    newCmdObj = newCmdWithWriteConcernBuilder.done();
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             // internalRenameIfOptionsAndIndexesMatch is adminOnly.
                                             DatabaseName::kAdmin.db(),
                                             std::move(cachedDbInfo),
                                             newCmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kNoRetry);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << newCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << newCmdObj);
}

BSONObj ShardServerProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    if (nss.isNamespaceAlwaysUnsharded()) {
        return getCollectionOptionsLocally(opCtx, nss);
    }

    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, nss.db()));
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cachedDbInfo->getPrimary()));

    const BSONObj filterObj = BSON("name" << nss.coll());
    const BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << filterObj);

    Shard::QueryResponse resultCollections;
    try {
        resultCollections = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              nss.db().toString(),
                                              appendDbVersionIfPresent(cmdObj, cachedDbInfo),
                                              Milliseconds(-1)));
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return BSONObj{};
    }

    if (resultCollections.docs.empty()) {
        return BSONObj{};
    }

    for (const BSONObj& bsonObj : resultCollections.docs) {
        // Return first element which matches on name and has options.
        const BSONElement nameElement = bsonObj["name"];
        if (!nameElement || nameElement.valueStringDataSafe() != nss.coll()) {
            continue;
        }

        const BSONElement optionsElement = bsonObj["options"];
        if (optionsElement) {
            auto optionObj = optionsElement.Obj();

            // If the BSON object has field 'info' and the BSON element 'info' has field 'uuid',
            // then extract the uuid and add to the BSON object to be return. This will ensure that
            // the BSON object is complaint with the BSON object returned for non-sharded namespace.
            if (auto infoElement = bsonObj["info"]; infoElement && infoElement["uuid"]) {
                return optionObj.addField(infoElement["uuid"]);
            }

            return optionObj.getOwned();
        }

        tassert(5983900,
                str::stream() << "Expected at most one collection with the name "
                              << nss.toStringForErrorMsg() << ": " << resultCollections.docs.size(),
                resultCollections.docs.size() <= 1);
    }

    return BSONObj{};
}

std::list<BSONObj> ShardServerProcessInterface::getIndexSpecs(OperationContext* opCtx,
                                                              const NamespaceString& ns,
                                                              bool includeBuildUUIDs) {
    // Note that 'ns' must be an unsharded collection. The indexes for a sharded collection must be
    // read from a shard with a chunk instead of the primary shard.
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    auto shard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, cachedDbInfo->getPrimary()));
    auto cmdObj = BSON("listIndexes" << ns.coll());
    Shard::QueryResponse indexes;
    try {
        indexes = uassertStatusOK(
            shard->runExhaustiveCursorCommand(opCtx,
                                              ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                              ns.db().toString(),
                                              appendDbVersionIfPresent(cmdObj, cachedDbInfo),
                                              Milliseconds(-1)));
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return std::list<BSONObj>();
    }
    return std::list<BSONObj>(indexes.docs.begin(), indexes.docs.end());
}

void ShardServerProcessInterface::createCollection(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   const BSONObj& cmdObj) {
    auto cachedDbInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbName.toStringWithTenantId()));
    BSONObjBuilder finalCmdBuilder(cmdObj);
    finalCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
    BSONObj finalCmdObj = finalCmdBuilder.obj();
    // TODO SERVER-67411 change executeCommandAgainstDatabasePrimary to take in DatabaseName
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             dbName.toStringWithTenantId(),
                                             std::move(cachedDbInfo),
                                             finalCmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << finalCmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << finalCmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << finalCmdObj);
}

void ShardServerProcessInterface::createIndexesOnEmptyCollection(
    OperationContext* opCtx, const NamespaceString& ns, const std::vector<BSONObj>& indexSpecs) {
    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), ns.db());
    router.route(
        opCtx,
        "copying index for empty collection {}"_format(ns.ns()),
        [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
            BSONObjBuilder cmdBuilder;
            cmdBuilder.append("createIndexes", ns.coll());
            cmdBuilder.append("indexes", indexSpecs);
            cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                              opCtx->getWriteConcern().toBSON());
            sharding::router::DBPrimaryRouter::appendCRUDUnshardedRoutingTokenToCommand(
                cdb->getPrimary(), cdb->getVersion(), &cmdBuilder);

            auto cmdObj = cmdBuilder.obj();

            auto response = std::move(
                gatherResponses(opCtx,
                                ns.db(),
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                Shard::RetryPolicy::kIdempotent,
                                std::vector<AsyncRequestsSender::Request>{
                                    AsyncRequestsSender::Request(cdb->getPrimary(), cmdObj)})
                    .front());

            uassertStatusOKWithContext(response.swResponse,
                                       str::stream() << "command was not sent " << cmdObj);
            const auto& result = response.swResponse.getValue().data;
            uassertStatusOKWithContext(getStatusFromCommandResult(result),
                                       str::stream() << "command was sent but failed " << cmdObj);
            uassertStatusOKWithContext(
                getWriteConcernStatusFromCommandResult(result),
                str::stream()
                    << "command was sent and succeeded, but failed waiting for write concern "
                    << cmdObj);
        });
}

void ShardServerProcessInterface::dropCollection(OperationContext* opCtx,
                                                 const NamespaceString& ns) {
    // Build and execute the dropCollection command against the primary shard of the given
    // database.
    auto cachedDbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, ns.db()));
    BSONObjBuilder newCmdBuilder;
    newCmdBuilder.append("drop", ns.coll());
    newCmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                         opCtx->getWriteConcern().toBSON());
    auto cmdObj = newCmdBuilder.done();
    auto response =
        executeCommandAgainstDatabasePrimary(opCtx,
                                             ns.db(),
                                             std::move(cachedDbInfo),
                                             cmdObj,
                                             ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                             Shard::RetryPolicy::kIdempotent);
    uassertStatusOKWithContext(response.swResponse,
                               str::stream() << "failed while running command " << cmdObj);
    auto result = response.swResponse.getValue().data;
    uassertStatusOKWithContext(getStatusFromCommandResult(result),
                               str::stream() << "failed while running command " << cmdObj);
    uassertStatusOKWithContext(getWriteConcernStatusFromCommandResult(result),
                               str::stream()
                                   << "write concern failed while running command " << cmdObj);
}

std::unique_ptr<Pipeline, PipelineDeleter>
ShardServerProcessInterface::attachCursorSourceToPipeline(Pipeline* ownedPipeline,
                                                          ShardTargetingPolicy shardTargetingPolicy,
                                                          boost::optional<BSONObj> readConcern) {
    return sharded_agg_helpers::attachCursorToPipeline(
        ownedPipeline, shardTargetingPolicy, std::move(readConcern));
}

std::unique_ptr<Pipeline, PipelineDeleter>
ShardServerProcessInterface::attachCursorSourceToPipeline(
    const AggregateCommandRequest& aggRequest,
    Pipeline* pipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<BSONObj> shardCursorsSortSpec,
    ShardTargetingPolicy shardTargetingPolicy,
    boost::optional<BSONObj> readConcern) {
    std::unique_ptr<Pipeline, PipelineDeleter> targetPipeline(pipeline,
                                                              PipelineDeleter(expCtx->opCtx));
    return sharded_agg_helpers::targetShardsAndAddMergeCursors(
        expCtx,
        std::make_pair(aggRequest, std::move(targetPipeline)),
        shardCursorsSortSpec,
        shardTargetingPolicy,
        std::move(readConcern));
}

std::unique_ptr<MongoProcessInterface::ScopedExpectUnshardedCollection>
ShardServerProcessInterface::expectUnshardedCollectionInScope(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<DatabaseVersion>& dbVersion) {
    class ScopedExpectUnshardedCollectionImpl : public ScopedExpectUnshardedCollection {
    public:
        ScopedExpectUnshardedCollectionImpl(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const boost::optional<DatabaseVersion>& dbVersion)
            : _expectUnsharded(opCtx, nss, ShardVersion::UNSHARDED(), dbVersion) {}

    private:
        ScopedSetShardRole _expectUnsharded;
    };

    return std::make_unique<ScopedExpectUnshardedCollectionImpl>(opCtx, nss, dbVersion);
}

void ShardServerProcessInterface::checkOnPrimaryShardForDb(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    DatabaseShardingState::assertIsPrimaryShardForDb(opCtx, DatabaseName{nss.db()});
}

}  // namespace mongo
