/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_feature_flag_gen.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

std::unique_ptr<CollatorInterface> getDefaultCollator(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    AutoGetCollectionForReadCommand collection(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Cannot analyze a shard key for a non-existing collection",
            collection);

    if (auto defaultCollator = collection->getDefaultCollator()) {
        return defaultCollator->clone();
    }
    return nullptr;
}

/**
 * Fetches the split point documents and applies 'callbackFn' to each of the documents. On a sharded
 * cluster, fetches the documents from the 'splitPointsShard'. On a standalone replica set, fetches
 * the documents locally.
 */
void fetchSplitPoints(OperationContext* opCtx,
                      const NamespaceString& splitPointsNss,
                      const Timestamp& splitPointsAfterClusterTime,
                      boost::optional<ShardId> splitPointsShard,
                      std::function<void(const BSONObj&)> callbackFn) {
    auto sort = BSON(AnalyzeShardKeySplitPointDocument::kSplitPointFieldName << 1);
    auto readConcern = repl::ReadConcernArgs(LogicalTime{splitPointsAfterClusterTime},
                                             repl::ReadConcernLevel::kLocalReadConcern);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        uassert(ErrorCodes::InvalidOptions,
                "The id of the shard that contains the temporary collection storing the split "
                "points for the shard key must be specified when running on a sharded cluster",
                splitPointsShard);
        auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, *splitPointsShard));

        std::vector<BSONObj> pipeline;
        pipeline.push_back(BSON("$match" << BSONObj()));
        pipeline.push_back(BSON("$sort" << sort));
        AggregateCommandRequest aggRequest(splitPointsNss, pipeline);
        aggRequest.setReadConcern(readConcern.toBSONInner());
        aggRequest.setWriteConcern(WriteConcernOptions());
        aggRequest.setUnwrappedReadPref(ReadPreferenceSetting::get(opCtx).toContainingBSON());

        uassertStatusOK(shard->runAggregation(
            opCtx,
            aggRequest,
            [&](const std::vector<BSONObj>& docs, const boost::optional<BSONObj>&) -> bool {
                for (const auto& doc : docs) {
                    callbackFn(doc);
                }
                return true;
            }));
    } else {
        uassertStatusOK(
            repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(opCtx, readConcern));

        DBDirectClient client(opCtx);
        FindCommandRequest findRequest(splitPointsNss);
        findRequest.setSort(sort);
        auto cursor = client.find(std::move(findRequest));
        while (cursor->more()) {
            callbackFn(cursor->next());
        }
    }
}

/**
 * Creates a CollectionRoutingInfoTargeter based on the split point documents in the
 * 'splitPointsNss' collection.
 */
CollectionRoutingInfoTargeter makeCollectionRoutingInfoTargeter(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const KeyPattern& shardKey,
    const NamespaceString& splitPointsNss,
    const Timestamp& splitPointsAfterClusterTime,
    boost::optional<ShardId> splitPointsShard) {
    std::vector<ChunkType> chunks;

    // This is a synthetic routing table so it doesn't matter what chunk version and shard id each
    // chunk below has.
    const auto collUuid = UUID::gen();
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    ChunkVersion version({OID::gen(), validAfter}, {1, 0});
    auto lastChunkMax = shardKey.globalMin();

    auto appendChunk = [&](const BSONObj& chunkMin, const BSONObj& chunkMax) {
        auto chunkNum = chunks.size();
        ChunkType chunk(collUuid,
                        {chunkMin.getOwned(), chunkMax.getOwned()},
                        version,
                        ShardId{str::stream() << chunkNum});
        chunk.setName(OID::gen());
        chunks.push_back(chunk);

        version.incMajor();
        lastChunkMax = chunk.getMax();
    };

    fetchSplitPoints(
        opCtx,
        splitPointsNss,
        splitPointsAfterClusterTime,
        splitPointsShard,
        [&](const BSONObj& doc) {
            auto splitPointDoc = AnalyzeShardKeySplitPointDocument::parse(
                IDLParserContext(DocumentSourceAnalyzeShardKeyReadWriteDistribution::kStageName),
                doc);
            auto splitPoint = splitPointDoc.getSplitPoint();
            uassertShardKeyValueNotContainArrays(splitPoint);
            appendChunk(lastChunkMax, splitPoint);
        });

    appendChunk(lastChunkMax, shardKey.globalMax());

    auto routingTableHistory = RoutingTableHistory::makeNew(nss,
                                                            collUuid,
                                                            shardKey,
                                                            getDefaultCollator(opCtx, nss),
                                                            false /* unique */,
                                                            OID::gen(),
                                                            validAfter,
                                                            boost::none /* timeseriesFields */,
                                                            boost::none /* reshardingFields */,
                                                            true /* allowMigrations */,
                                                            chunks);

    auto cm = ChunkManager(ShardId("0"),
                           DatabaseVersion(UUID::gen(), validAfter),
                           RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(
                               std::move(routingTableHistory))),
                           boost::none);

    return CollectionRoutingInfoTargeter(CollectionRoutingInfo{
        std::move(cm), boost::optional<ShardingIndexesCatalogCache>(boost::none)});
}

/**
 * Calculates the read and write distribution metrics for the collection 'collUuid' based on its
 * sampled queries.
 */
void processSampledQueries(OperationContext* opCtx,
                           ReadDistributionMetricsCalculator* readDistributionCalculator,
                           WriteDistributionMetricsCalculator* writeDistributionCalculator,
                           const UUID& collUuid) {
    FindCommandRequest findRequest{NamespaceString::kConfigSampledQueriesNamespace};
    findRequest.setFilter(BSON(SampledQueryDocument::kCollectionUuidFieldName << collUuid));

    DBDirectClient client(opCtx);
    auto cursor = client.find(std::move(findRequest));

    while (cursor->more()) {
        const auto obj = cursor->next().getOwned();
        const auto doc = SampledQueryDocument::parse(
            IDLParserContext(DocumentSourceAnalyzeShardKeyReadWriteDistribution::kStageName), obj);

        switch (doc.getCmdName()) {
            case SampledCommandNameEnum::kFind:
            case SampledCommandNameEnum::kAggregate:
            case SampledCommandNameEnum::kDistinct:
            case SampledCommandNameEnum::kCount: {
                readDistributionCalculator->addQuery(opCtx, doc);
                break;
            }
            case SampledCommandNameEnum::kUpdate:
            case SampledCommandNameEnum::kDelete:
            case SampledCommandNameEnum::kFindAndModify: {
                writeDistributionCalculator->addQuery(opCtx, doc);
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
}

/**
 * Calculates the read and write distribution metrics for the collection 'collUuid' based on its
 * sampled diffs. Currently, this only involves calculating the number of shard key updates.
 */
void processSampledDiffs(OperationContext* opCtx,
                         ReadDistributionMetricsCalculator* readDistributionCalculator,
                         WriteDistributionMetricsCalculator* writeDistributionCalculator,
                         const UUID& collUuid,
                         const KeyPattern& shardKey) {
    std::vector<BSONObj> pipeline;

    BSONObjBuilder orBuilder;
    BSONArrayBuilder orArrayBuilder(orBuilder.subarrayStart("$or"));
    for (const auto& element : shardKey.toBSON()) {
        const auto shardKeyFieldName = element.fieldNameStringData();
        const auto path = SampledQueryDiffDocument::kDiffFieldName + "." + shardKeyFieldName;
        orArrayBuilder.append(BSON(path << BSON("$exists" << true)));

        size_t startIndex = 0;
        while (startIndex < shardKeyFieldName.size()) {
            const size_t lastDotIndex = shardKeyFieldName.find(".", startIndex);
            if (lastDotIndex == std::string::npos) {
                break;
            }

            BSONObjBuilder andBuilder;
            BSONArrayBuilder andArrayBuilder(andBuilder.subarrayStart("$and"));
            const auto shardKeyPrefixFieldName = shardKeyFieldName.substr(0, lastDotIndex);
            const auto prefixPath =
                SampledQueryDiffDocument::kDiffFieldName + "." + shardKeyPrefixFieldName;
            andArrayBuilder.append(BSON(prefixPath << BSON("$exists" << true)));
            andArrayBuilder.append(BSON(prefixPath << BSON("$not" << BSON("$type"
                                                                          << "object"))));
            andArrayBuilder.done();
            orArrayBuilder.append(andBuilder.done());

            startIndex = lastDotIndex + 1;
        }
    }
    orArrayBuilder.done();

    pipeline.push_back(BSON(
        "$match" << BSON("$and" << BSON_ARRAY(
                             BSON(SampledQueryDiffDocument::kCollectionUuidFieldName << collUuid)
                             << orBuilder.done()))));
    pipeline.push_back(BSON("$count" << WriteDistributionMetrics::kNumShardKeyUpdatesFieldName));
    AggregateCommandRequest aggRequest(NamespaceString::kConfigSampledQueriesDiffNamespace,
                                       pipeline);

    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, true /* secondaryOk */, false /* useExhaust*/));

    if (cursor->more()) {
        const auto doc = cursor->next();
        const auto numShardKeyUpdates =
            doc.getField(WriteDistributionMetrics::kNumShardKeyUpdatesFieldName).exactNumberLong();
        writeDistributionCalculator->setNumShardKeyUpdates(numShardKeyUpdates);
    }

    invariant(!cursor->more());
}

}  // namespace

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(
    _analyzeShardKeyReadWriteDistribution,
    DocumentSourceAnalyzeShardKeyReadWriteDistribution::LiteParsed::parse,
    DocumentSourceAnalyzeShardKeyReadWriteDistribution::createFromBson,
    AllowedWithApiStrict::kNeverInVersion1,
    analyze_shard_key::gFeatureFlagAnalyzeShardKey);

boost::intrusive_ptr<DocumentSource>
DocumentSourceAnalyzeShardKeyReadWriteDistribution::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(6875701,
            str::stream() << kStageName << " must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    auto spec = DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec::parse(
        IDLParserContext(kStageName), specElem.embeddedObject());

    return make_intrusive<DocumentSourceAnalyzeShardKeyReadWriteDistribution>(pExpCtx,
                                                                              std::move(spec));
}

Value DocumentSourceAnalyzeShardKeyReadWriteDistribution::serialize(
    SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484305);
    }

    return Value(Document{{getSourceName(), _spec.toBSON()}});
}

DocumentSource::GetNextResult DocumentSourceAnalyzeShardKeyReadWriteDistribution::doGetNext() {
    if (_finished) {
        return GetNextResult::makeEOF();
    }

    _finished = true;

    auto collUuid = uassertStatusOK(validateCollectionOptionsLocally(pExpCtx->opCtx, pExpCtx->ns));
    auto targeter = makeCollectionRoutingInfoTargeter(pExpCtx->opCtx,
                                                      pExpCtx->ns,
                                                      _spec.getKey(),
                                                      _spec.getSplitPointsNss(),
                                                      _spec.getSplitPointsAfterClusterTime(),
                                                      _spec.getSplitPointsShardId());
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    processSampledDiffs(pExpCtx->opCtx,
                        &readDistributionCalculator,
                        &writeDistributionCalculator,
                        collUuid,
                        _spec.getKey());
    processSampledQueries(
        pExpCtx->opCtx, &readDistributionCalculator, &writeDistributionCalculator, collUuid);

    // The config.sampledQueries and config.sampleQueriesDiff collections are not written to (and
    // read from) transactionally so it is possible for the number of shard key updates found above
    // to be greater than the total number of writes. Therefore, we need to cap it in order to to
    // keep the percentage between 0 and 100.
    writeDistributionCalculator.setNumShardKeyUpdates(
        std::min(writeDistributionCalculator.getNumShardKeyUpdates(),
                 writeDistributionCalculator.getNumTotal()));

    DocumentSourceAnalyzeShardKeyReadWriteDistributionResponse response;
    response.setReadDistribution(readDistributionCalculator.getMetrics());
    response.setWriteDistribution(writeDistributionCalculator.getMetrics());
    return {Document(response.toBSON())};
}

}  // namespace analyze_shard_key
}  // namespace mongo
