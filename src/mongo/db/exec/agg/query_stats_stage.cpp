// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/query_stats_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/query_stats_failed_to_record_info.h"
#include "mongo/db/query/query_stats/query_stats_top_k_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/util/buildinfo.h"

#include <deque>
#include <string_view>
#include <utility>

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
        queryStatsDS->serialize().getDocument().toBson(),
        queryStatsDS->_topKSortSpec);
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

auto& queryStatsTopKOptimizations = *MetricBuilder<Counter64>{"queryStats.numTopKOptimizations"};
}  // namespace

QueryStatsStage::QueryStatsStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 TransformAlgorithmEnum algorithm,
                                 std::string hmacKey,
                                 BSONObj serializedForDebug,
                                 boost::optional<query_stats::TopKSortSpec> topKSortSpec)
    : Stage(stageName, expCtx),
      _currentCopiedPartition(0),
      _algorithm{algorithm},
      _hmacKey{hmacKey},
      _serializedForDebug{serializedForDebug.getOwned()},
      _topKSortSpec{std::move(topKSortSpec)} {}

BSONObj QueryStatsStage::computeQueryStatsKey(
    std::shared_ptr<const Key> key, const SerializationContext& serializationContext) const {
    static const auto sha256HmacStringDataHasher = [](const std::string& key, std::string_view sd) {
        auto hashed = SHA256Block::computeHmac(
            (const uint8_t*)key.data(), key.size(), (const uint8_t*)sd.data(), sd.size());
        return hashed.toString();
    };

    auto opts = query_shape::SerializationOptions{};
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToDebugTypeString;
    if (_algorithm == TransformAlgorithmEnum::kHmacSha256) {
        opts.transformIdentifiers = true;
        opts.transformIdentifiersCallback = [&](std::string_view sd) {
            return sha256HmacStringDataHasher(_hmacKey, sd);
        };
    }
    return key->toBson(pExpCtx->getOperationContext(), opts, serializationContext);
}

void QueryStatsStage::conditionallyLogOutput(const Document& doc) const {
    if (_algorithm != TransformAlgorithmEnum::kNone) {
        LOGV2_DEBUG_OPTIONS(7808301,
                            3,
                            {logv2::LogTruncation::Disabled},
                            "Logging all outputs of $queryStats",
                            "thisOutput"_attr = doc);
    }
}

void QueryStatsStage::conditionallyLogFinished() const {
    if (_algorithm != TransformAlgorithmEnum::kNone) {
        LOGV2_DEBUG_OPTIONS(
            7808302, 3, {logv2::LogTruncation::Disabled}, "Finished logging output of $queryStats");
    }
}

GetNextResult QueryStatsStage::doGetNext() {
    auto& queryStatsStore = getQueryStatsStore(getContext()->getOperationContext());

    // Top-K fast path: do one full cheap scan to identify candidates, then materialize only them.
    if (_topKSortSpec) {
        if (auto doc = nextTopKDocument(queryStatsStore, *_topKSortSpec)) {
            return std::move(*doc);
        }
        // Every candidate materialized, so the retained $sort + $limit already has the top K.
        if (!_topKCandidateFailed) {
            conditionallyLogFinished();
            return GetNextResult::makeEOF();
        }
        // Resort to the full store scan, if we did not materialize enough documents in the top-k
        // optimization. The $sort/$limit that follows this stage will ensure the documents are
        // returned in the correct order.
        // TODO SERVER-136027: revisit whether the $sort/$limit can be removed once they are no
        // longer needed here.
    }

    return nextFullScanDocument(queryStatsStore);
}

GetNextResult QueryStatsStage::nextFullScanDocument(QueryStatsStore& queryStatsStore) {
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
    while (_currentCopiedPartition.isValidPartitionId(queryStatsStore.numPartitions())) {
        if (!_currentCopiedPartition.isLoaded()) {
            _currentCopiedPartition.load(queryStatsStore, _topKConsumedKeys);
        }
        // CopiedPartition::load() will throw if any errors occur.
        // Safe to assume _currentCopiedPartition is now loaded.

        // Exhaust all elements in the current copied partition.
        if (auto doc = nextDocument(_currentCopiedPartition.statsEntries,
                                    _currentCopiedPartition.getReadTimestamp())) {
            return std::move(*doc);
        }
        // Once we have exhausted entries in this partition, move on to the next partition.
        _currentCopiedPartition.incrementPartitionId();
    }

    conditionallyLogFinished();
    return GetNextResult::makeEOF();
}

boost::optional<Document> QueryStatsStage::nextDocument(std::deque<QueryStatsEntry>& statsEntries,
                                                        const Date_t& readTimestamp) const {
    // Use a while loop here to handle cases where toDocument() may fail for a specific
    // QueryStatsEntry, in which case we suppress the thrown exception and continue iterating to the
    // next available entry.
    while (!statsEntries.empty()) {
        const auto& queryStatsEntry = statsEntries.front();
        ON_BLOCK_EXIT([&statsEntries]() { statsEntries.pop_front(); });
        if (auto doc = toDocument(readTimestamp, queryStatsEntry)) {
            conditionallyLogOutput(*doc);
            return doc;
        }
    }
    return boost::none;
}

boost::optional<Document> QueryStatsStage::toDocument(
    const Date_t& readTimestamp, const QueryStatsEntry& queryStatsEntry) const {
    const auto& key = queryStatsEntry.key;
    try {
        auto queryStatsKey = computeQueryStatsKey(key, SerializationContext::stateDefault());
        // Skip entries where the key is over BSONObjMaxUserSize or too deeply nested for the reply.
        uassertStatusOK(query_stats::validateQueryStatsKeyBson(queryStatsKey));
        // We use the representative shape to generate the key and shape hashes. This avoids
        // returning duplicate hashes if we have bugs that cause two different representative shapes
        // to re-parse into the same debug shape.
        auto keyHash = computeKeyHashString(pExpCtx->getOperationContext(), *key);
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

        bool includeWriteMetrics =
            feature_flags::gFeatureFlagQueryStatsUpdateCommand
                .isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(pExpCtx->getOperationContext()),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        bool includeCBRMetrics =
            feature_flags::gFeatureFlagQueryStatsCBRMetrics.isEnabledUseLastLTSFCVWhenUninitialized(
                VersionContext::getDecoration(pExpCtx->getOperationContext()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        bool useQueryStatsWithSubsectionsFormat =
            feature_flags::gFeatureFlagQueryStatsMetricsSubsections.isEnabled();
        bool includeErrorMetrics = feature_flags::gFeatureFlagQueryStatsErrors.checkEnabled();
        return Document{{"key", std::move(queryStatsKey)},
                        {"keyHash", keyHash},
                        {"queryShapeHash", queryShapeHash},
                        {"metrics",
                         queryStatsEntry.toBSON(useQueryStatsWithSubsectionsFormat,
                                                includeWriteMetrics,
                                                includeCBRMetrics,
                                                includeErrorMetrics)},
                        {"asOf", readTimestamp}};
    } catch (const DBException& ex) {
        queryStatsHmacApplicationErrors.increment();
        const auto& hash = absl::HashOf(key);
        const auto queryShape = key->universalComponents()._queryShape->toBson(
            pExpCtx->getOperationContext(),
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
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
        // investigated / fixed:
        //  - The QueryFeatureNotAllowed error occurs when a query was run (and the query stats were
        //  recorded) that needed a higher FCV, but later the cluster FCV was dropped, and then the
        //  query stats were requested and the server can no longer parse that query that needed the
        //  higher FCV.
        //  - The BSONObjectTooLarge error can occur if a test issues a very large query, which some
        //  tests do on purpose. The query shape reported here can be bigger than the original query
        //  due to hmac application or other format changes. So this is not a concerning failure
        //  mode.
        //  - Error code 10071200 occurs when a query referencing the $$CLUSTER_TIME system
        //  variable errored on a standalone node, since it is unavailable outside of replica
        //  sets/sharded clusters. Re-parsing later on the same standalone node reliably hits the
        //  same restriction. TODO SERVER-132688: Remove this exclusion.
        //  - The Overflow error occurs when the shape is too deeply nested to be wrapped in the
        //  reply, which some tests do on purpose. Skipping the entry is the intended behavior.
        if ((kDebugBuild || internalQueryStatsErrorsAreCommandFatal.load()) &&
            ex.code() != ErrorCodes::QueryFeatureNotAllowed &&
            ex.code() != ErrorCodes::BSONObjectTooLarge &&  // query shape too large
            ex.code() != 10071200 &&              // $$CLUSTER_TIME unavailable in standalone mode
            ex.code() != ErrorCodes::Overflow &&  // query shape too deeply nested
            ex.code() != 16490                    // document grew too large - before hitting BSON
        ) {
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

void QueryStatsStage::computeTopKCandidates(const QueryStatsStore& queryStatsStore,
                                            const TopKSortSpec& spec) {
    queryStatsTopKOptimizations.increment();

    auto accessor = query_stats::getCheapMetricAccessor(spec.dottedMetricsPath);
    tassert(12938701, "TopKSortSpec set for unsupported path", accessor);

    // The scan locks one partition at a time and only reads the cheap metric.
    _topKScanTimestamp = Date_t::now();
    _topKCandidateEntries = queryStatsStore.topKCandidateEntries(
        spec.limit,
        *accessor,
        [&spec](int64_t a, int64_t b) { return spec.isBetter(a, b); },
        // Check for interrupts between partitions, since 'topKCandidateEntries' scans the entire
        // store.
        [this] { pExpCtx->checkForInterrupt(); });
}

boost::optional<Document> QueryStatsStage::nextTopKDocument(QueryStatsStore& queryStatsStore,
                                                            const TopKSortSpec& spec) {
    if (!_topKCandidateEntries.has_value()) {
        computeTopKCandidates(queryStatsStore, spec);
    }

    while (!_topKCandidateEntries->empty()) {
        ON_BLOCK_EXIT([this] { _topKCandidateEntries->pop_back(); });
        const auto& [key, queryStatsEntry] = _topKCandidateEntries->back();
        _topKConsumedKeys.insert(key);

        if (auto doc = toDocument(_topKScanTimestamp, queryStatsEntry)) {
            conditionallyLogOutput(*doc);
            return doc;
        }
        _topKCandidateFailed = true;
    }
    return boost::none;
}

/**
 * Loads the current CopiedPartition with copies of the QueryStatsEntries located in partition of
 * cache corresponding to the partitionId of the current CopiedPartition. This ensures that the
 * partition mutex is only held for the duration of copying.
 */
void QueryStatsStage::CopiedPartition::load(QueryStatsStore& queryStatsStore,
                                            const stdx::unordered_set<std::size_t>& skipKeys) {
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
            if (!skipKeys.contains(hash)) {
                statsEntries.push_back(metrics);
            }
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

}  // namespace exec::agg
}  // namespace mongo
