/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_collection_cloner.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo {

ReshardingCollectionCloner::ReshardingCollectionCloner(ShardKeyPattern newShardKeyPattern,
                                                       NamespaceString sourceNss,
                                                       CollectionUUID sourceUUID)
    : _newShardKeyPattern(std::move(newShardKeyPattern)),
      _sourceNss(std::move(sourceNss)),
      _sourceUUID(std::move(sourceUUID)) {}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingCollectionCloner::buildCursor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ShardId& recipientShard,
    Timestamp atClusterTime,
    const NamespaceString& outputNss) {

    std::vector<BSONObj> serializedPipeline =
        createAggForCollectionCloning(expCtx, _newShardKeyPattern, outputNss, recipientShard)
            ->serializeToBson();

    AggregationRequest request(_sourceNss, std::move(serializedPipeline));
    request.setCollectionUUID(_sourceUUID);
    request.setHint(BSON("_id" << 1));
    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << atClusterTime));

    return sharded_agg_helpers::targetShardsAndAddMergeCursors(std::move(expCtx),
                                                               std::move(request));
}

void ReshardingCollectionCloner::runPipeline(OperationContext* opCtx,
                                             const ShardId& recipientShard,
                                             Timestamp atClusterTime,
                                             const NamespaceString& outputNss) {
    // Assume that the input collection isn't a view. The collectionUUID parameter to the aggregate
    // would enforce this anyway.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_sourceNss.coll()] = {_sourceNss, std::vector<BSONObj>{}};

    // Assume that the config.cache.chunks collection isn't a view either.
    auto tempNss = constructTemporaryReshardingNss(_sourceNss.db(), _sourceUUID);
    auto tempCacheChunksNss =
        NamespaceString(NamespaceString::kConfigDb, "cache.chunks." + tempNss.ns());
    resolvedNamespaces[tempCacheChunksNss.coll()] = {tempCacheChunksNss, std::vector<BSONObj>{}};

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    _sourceNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    MongoProcessInterface::create(opCtx),
                                                    std::move(resolvedNamespaces),
                                                    _sourceUUID);

    auto pipeline = buildCursor(expCtx, recipientShard, atClusterTime, outputNss);
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    auto doc = pipeline->getNext();
    while (doc) {
        auto obj = doc->toBson();

        // TODO: Do some amount of batching for inserts.
        writeConflictRetry(opCtx, "reshardingCollectionClonerInsertDocument", outputNss.ns(), [&] {
            AutoGetCollection outputColl(opCtx, outputNss, MODE_IX);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Resharding collection cloner's output collection '"
                                  << outputNss << "' did not already exist",
                    outputColl);
            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(outputColl->insertDocument(opCtx, InsertStatement{obj}, nullptr));
            wuow.commit();
        });

        doc = pipeline->getNext();
    }
}

}  // namespace mongo
