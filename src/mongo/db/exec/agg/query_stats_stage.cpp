/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/query_stats_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/query_stats_failed_to_record_info.h"
#include "mongo/logv2/log.h"
#include "mongo/util/buildinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceQueryStatsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* queryStatsDS = dynamic_cast<DocumentSourceQueryStats*>(documentSource.get());

    tassert(10965500, "expected 'DocumentSourceQueryStats' type", queryStatsDS);

    return make_intrusive<exec::agg::QueryStatsStage>(
        queryStatsDS->kStageName,
        queryStatsDS->getExpCtx(),
        queryStatsDS->_algorithm,
        queryStatsDS->_hmacKey,
        queryStatsDS->serialize().getDocument().toBson());
}

REGISTER_AGG_STAGE_MAPPING(queryStats,
                           DocumentSourceQueryStats::id,
                           documentSourceQueryStatsToStageFn);

namespace exec::agg {

// Fail point to mimic re-parsing errors during execution.
MONGO_FAIL_POINT_DEFINE(queryStatsFailToReparseQueryShape);

// Fail point to mimic getting a ErrorCodes::QueryFeatureNotAllowed exception during re-parsing.
MONGO_FAIL_POINT_DEFINE(queryStatsGenerateQueryFeatureNotAllowedError);

namespace {
auto& queryStatsHmacApplicationErrors =
    *MetricBuilder<Counter64>{"queryStats.numHmacApplicationErrors"};
}

QueryStatsStage::QueryStatsStage(StringData stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 TransformAlgorithmEnum algorithm,
                                 std::string hmacKey,
                                 BSONObj serializedForDebug)
    : Stage(stageName, expCtx),
      _currentCopiedPartition(0),
      _algorithm{algorithm},
      _hmacKey{hmacKey},
      _serializedForDebug{serializedForDebug.getOwned()} {}

BSONObj QueryStatsStage::computeQueryStatsKey(
    std::shared_ptr<const Key> key, const SerializationContext& serializationContext) const {
    static const auto sha256HmacStringDataHasher = [](const std::string& key, StringData sd) {
        auto hashed = SHA256Block::computeHmac(
            (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.data(), sd.size());
        return hashed.toString();
    };

    auto opts = SerializationOptions{};
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    if (_algorithm == TransformAlgorithmEnum::kHmacSha256) {
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = [&](StringData sd) {
            return sha256HmacStringDataHasher(_hmacKey, sd);
        };
    }
    return key->toBson(pExpCtx->getOperationContext(), opts, serializationContext);
}

GetNextResult QueryStatsStage::doGetNext() {
    const auto shouldLog = _algorithm != TransformAlgorithmEnum::kNone;
    /**
     * When a CopiedPartition is present (loaded) and contains more elements (QueryStatsEntry), we
     * can process and return the next element in the _currentCopiedPartition.
     *
     * When the current CopiedPartition is exhausted (emptied), we move on to the next
     * partition. Once we have iterated to the end of the valid partitions, we are done iterating
     * over all the queryStatsStore entries.
     *
     * We iterate over a copied container (CopiedPartition) containing the entries in
     * the partition to reduce the time under which the partition lock is held.
     */
    auto& queryStatsStore = getQueryStatsStore(getContext()->getOperationContext());

    while (_currentCopiedPartition.isValidPartitionId(queryStatsStore.numPartitions())) {
        if (!_currentCopiedPartition.isLoaded()) {
            _currentCopiedPartition.load(queryStatsStore);
        }
        // CopiedPartition::load() will throw if any errors occur.
        // Safe to assume _currentCopiedPartition is now loaded.

        // Exhaust all elements in the current copied partition.
        // Use a while loop here to handle cases where toDocument() may fail for a specific
        // QueryStatsEntry, in which case we suppress the thrown exception and continue
        // iterating to the next available entry.
        while (!_currentCopiedPartition.empty()) {
            auto& statsEntries = _currentCopiedPartition.statsEntries;
            const auto& queryStatsEntry = statsEntries.front();
            ON_BLOCK_EXIT([&statsEntries]() { statsEntries.pop_front(); });
            if (auto doc =
                    toDocument(_currentCopiedPartition.getReadTimestamp(), queryStatsEntry)) {
                if (shouldLog) {
                    LOGV2_DEBUG_OPTIONS(7808301,
                                        3,
                                        {logv2::LogTruncation::Disabled},
                                        "Logging all outputs of $queryStats",
                                        "thisOutput"_attr = *doc);
                }
                return std::move(*doc);
            }
        }
        // Once we have exhausted entries in this partition, move on to the next partition.
        _currentCopiedPartition.incrementPartitionId();
    }

    if (shouldLog) {
        LOGV2_DEBUG_OPTIONS(
            7808302, 3, {logv2::LogTruncation::Disabled}, "Finished logging output of $queryStats");
    }
    return DocumentSource::GetNextResult::makeEOF();
}

boost::optional<Document> QueryStatsStage::toDocument(
    const Date_t& partitionReadTime, const QueryStatsEntry& queryStatsEntry) const {
    const auto& key = queryStatsEntry.key;
    try {
        auto queryStatsKey = computeQueryStatsKey(key, SerializationContext::stateDefault());
        // We use the representative shape to generate the key and shape hashes. This avoids
        // returning duplicate hashes if we have bugs that cause two different representative shapes
        // to re-parse into the same debug shape.
        auto representativeShapeKey =
            key->toBson(pExpCtx->getOperationContext(),
                        SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                        SerializationContext::stateDefault());
        // This SHA256 version of the hash is output to aid in data analytics use cases. In these
        // cases, we often care about comparing hashes from different hosts, potentially on
        // different versions and platforms. The thinking here is that the SHA256 algorithm is more
        // stable across these different environments than the quicker 'absl::HashOf'
        // implementation.
        auto keyHash = SHA256Block::computeHash((const uint8_t*)representativeShapeKey.objdata(),
                                                representativeShapeKey.objsize())
                           .toString();
        auto queryShapeHash = key->getQueryShapeHash(pExpCtx->getOperationContext(),
                                                     SerializationContext::stateDefault())
                                  .toHexString();

        if (MONGO_unlikely(queryStatsGenerateQueryFeatureNotAllowedError.shouldFail())) {
            uasserted(ErrorCodes::QueryFeatureNotAllowed,
                      "queryStatsGenerateQueryFeatureNotAllowedError fail point is enabled");
        }

        if (MONGO_unlikely(queryStatsFailToReparseQueryShape.shouldFail())) {
            uasserted(ErrorCodes::FailPointEnabled,
                      "queryStatsFailToReparseQueryShape fail point is enabled");
        }

        return Document{{"key", std::move(queryStatsKey)},
                        {"keyHash", keyHash},
                        {"queryShapeHash", queryShapeHash},
                        {"metrics", queryStatsEntry.toBSON()},
                        {"asOf", partitionReadTime}};
    } catch (const DBException& ex) {
        queryStatsHmacApplicationErrors.increment();
        const auto& hash = absl::HashOf(key);
        const auto queryShape = key->universalComponents()._queryShape->toBson(
            pExpCtx->getOperationContext(),
            SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
            SerializationContext::stateDefault());
        LOGV2_DEBUG(7349403,
                    2,
                    "Error encountered when applying hmac to query shape, will not publish "
                    "queryStats for this entry.",
                    "status"_attr = ex.toStatus(),
                    "hash"_attr = hash,
                    "debugQueryShape"_attr = queryShape);

        // Normally, when we encounter and error when trying to pull out a query shape key
        // and compute its document to return as a stage result, we skip over the key and log.
        // However, when running in debug mode
        // (or the internalQueryStatsErrorsAreCommandFatal option is set) we want to fail the query
        // instead so that we can catch potential errors in query stats during testing / fuzzing
        // to investigate and resolve them.
        // Within this case however, we want to avoid failing on errors that are because the query
        // feature is disallowed, as these errors do not suggest that anything needs to be
        // investigated / fixed. The QueryFeatureNotAllowed error occurs when a query was run
        // (and the query stats were recorded) that needed a higher FCV, but later the cluster
        // FCV was dropped, and then the query stats were requested and the server can no longer
        // parse that query that needed the higher FCV.
        if ((kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) &&
            (ex.code() != ErrorCodes::QueryFeatureNotAllowed)) {
            auto keyString = std::to_string(hash);
            uasserted(Status{
                QueryStatsFailedToRecordInfo(
                    _serializedForDebug, ex.toStatus(), getBuildInfoVersionOnly().getVersion()),
                str::stream() << "Failed to re-parse query stats store key when reading. Hash: "
                              << keyString << ", Query Shape: " << queryShape.toString()});
        }
    }
    return {};
}

/**
 * Loads the current CopiedPartition with copies of the QueryStatsEntries located in partition of
 * cache corresponding to the partitionId of the current CopiedPartition. This ensures that the
 * partition mutex is only held for the duration of copying.
 */
void QueryStatsStage::CopiedPartition::load(QueryStatsStore& queryStatsStore) {
    tassert(7932100,
            "Attempted to load invalid partition.",
            _partitionId < queryStatsStore.numPartitions());
    tassert(7932101, "Partition was already loaded.", !isLoaded());
    // 'statsEntries' should be empty, clear just in case.
    statsEntries.clear();

    // Capture the time at which reading the partition begins.
    _readTimestamp = Date_t::now();
    {
        // We only keep the partition (which holds a lock)
        // for the time needed to collect the metrics (QueryStatsEntry)
        const auto partition = queryStatsStore.getPartition(_partitionId);

        // Note the intentional copy of QueryStatsEntry.
        // This will give us a snapshot of all the metrics we want to report.
        for (auto&& [hash, metrics] : *partition) {
            statsEntries.push_back(metrics);
        }
    }
    _isLoaded = true;
}

bool QueryStatsStage::CopiedPartition::isLoaded() const {
    return _isLoaded;
}

void QueryStatsStage::CopiedPartition::incrementPartitionId() {
    // Ensure loaded state is reset when partitionId is incremented.
    ++_partitionId;
    _isLoaded = false;
}

bool QueryStatsStage::CopiedPartition::isValidPartitionId(
    QueryStatsStore::PartitionId maxNumPartitions) const {
    return _partitionId < maxNumPartitions;
}

const Date_t& QueryStatsStage::CopiedPartition::getReadTimestamp() const {
    return _readTimestamp;
}

bool QueryStatsStage::CopiedPartition::empty() const {
    return statsEntries.empty();
}

}  // namespace exec::agg
}  // namespace mongo
