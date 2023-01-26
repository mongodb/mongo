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
 * Creates a CollectionRoutingInfoTargeter based on the split point documents in the
 * 'splitPointsNss' collection on the 'splitPointsShard'.
 */
CollectionRoutingInfoTargeter makeCollectionRoutingInfoTargeter(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const KeyPattern& shardKey,
    ShardId splitPointsShard,
    const NamespaceString& splitPointsNss,
    const Timestamp& splitPointsAfterClusterTime) {
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

    auto shard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, splitPointsShard));

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSONObj()));
    pipeline.push_back(
        BSON("$sort" << BSON(AnalyzeShardKeySplitPointDocument::kSplitPointFieldName << 1)));
    AggregateCommandRequest aggRequest(splitPointsNss, pipeline);
    aggRequest.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                   << repl::readConcernLevels::kLocalName
                                   << repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                   << splitPointsAfterClusterTime));
    aggRequest.setWriteConcern(WriteConcernOptions());

    uassertStatusOK(shard->runAggregation(
        opCtx,
        aggRequest,
        [&](const std::vector<BSONObj>& docs, const boost::optional<BSONObj>&) -> bool {
            for (const auto& doc : docs) {
                auto splitPointDoc = AnalyzeShardKeySplitPointDocument::parse(
                    IDLParserContext(
                        DocumentSourceAnalyzeShardKeyReadWriteDistribution::kStageName),
                    doc);
                appendChunk(lastChunkMax, splitPointDoc.getSplitPoint());
            }
            return true;
        }));
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
                                                            boost::none /* maxChunkSizeBytes */,
                                                            true /* allowMigrations */,
                                                            chunks);

    auto cm = ChunkManager(ShardId("0"),
                           DatabaseVersion(UUID::gen(), validAfter),
                           RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(
                               std::move(routingTableHistory))),
                           boost::none);

    return CollectionRoutingInfoTargeter(
        CollectionRoutingInfo{std::move(cm), boost::optional<GlobalIndexesCache>(boost::none)});
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
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{getSourceName(), _spec.toBSON()}});
}

DocumentSource::GetNextResult DocumentSourceAnalyzeShardKeyReadWriteDistribution::doGetNext() {
    if (_finished) {
        return GetNextResult::makeEOF();
    }

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Cannot analyze a shard key for a non-existing collection",
            pExpCtx->uuid);
    auto collUuid = *pExpCtx->uuid;
    _finished = true;

    auto targeter = makeCollectionRoutingInfoTargeter(pExpCtx->opCtx,
                                                      pExpCtx->ns,
                                                      _spec.getKey(),
                                                      _spec.getSplitPointsShardId(),
                                                      _spec.getSplitPointsNss(),
                                                      _spec.getSplitPointsAfterClusterTime());
    ReadDistributionMetricsCalculator readDistributionCalculator(targeter);
    WriteDistributionMetricsCalculator writeDistributionCalculator(targeter);

    processSampledQueries(
        pExpCtx->opCtx, &readDistributionCalculator, &writeDistributionCalculator, collUuid);

    DocumentSourceAnalyzeShardKeyReadWriteDistributionResponse response;
    response.setReadDistribution(readDistributionCalculator.getMetrics());
    response.setWriteDistribution(writeDistributionCalculator.getMetrics());
    return {Document(response.toBSON())};
}

}  // namespace analyze_shard_key
}  // namespace mongo
