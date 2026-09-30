// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/query_stats_top_k_metrics.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <deque>
#include <string_view>
#include <vector>

namespace mongo {

using namespace query_stats;

namespace exec::agg {

class QueryStatsStage final : public Stage {
public:
    QueryStatsStage(std::string_view stageName,
                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    TransformAlgorithmEnum algorithm,
                    std::string hmacKey,
                    BSONObj serializedForDebug,
                    boost::optional<query_stats::TopKSortSpec> topKSortSpec = boost::none);

private:
    /*
     * CopiedPartition: This struct is representative of a copied ("materialized") partition
     * which should be loaded from the QueryStatsStore. It is used to hold a copy of the
     * QueryStatsEntries corresponding to the provided partitionId.
     * Once a CopiedPartition has been loaded from QueryStatsStore, it provides access to the
     * QueryStatsEntries of the partition without requiring holding the lock over the partition in
     * the partitioned cache.
     */
    struct CopiedPartition {
        CopiedPartition(QueryStatsStore::PartitionId partitionId)
            : statsEntries(), _readTimestamp(), _partitionId(partitionId) {}

        ~CopiedPartition() = default;

        bool isLoaded() const;

        void incrementPartitionId();

        bool isValidPartitionId(QueryStatsStore::PartitionId maxNumPartitions) const;

        const Date_t& getReadTimestamp() const;

        // Copies every entry in the partition whose key is not in 'skipKeys'.
        void load(QueryStatsStore& queryStatsStore,
                  const stdx::unordered_set<std::size_t>& skipKeys);

        std::deque<QueryStatsEntry> statsEntries;

    private:
        Date_t _readTimestamp;
        QueryStatsStore::PartitionId _partitionId;
        bool _isLoaded{false};
    };

    GetNextResult doGetNext() final;

    /**
     * Returns the document for the front entry of 'statsEntries', popping the entries it consumes.
     * Skips over entries that fail to serialize, and returns boost::none once 'statsEntries' is
     * empty.
     */
    boost::optional<Document> nextDocument(std::deque<QueryStatsEntry>& statsEntries,
                                           const Date_t& readTimestamp) const;

    boost::optional<Document> toDocument(const Date_t& readTimestamp,
                                         const QueryStatsEntry& queryStatsEntry) const;

    void conditionallyLogOutput(const Document& doc) const;

    void conditionallyLogFinished() const;

    BSONObj computeQueryStatsKey(std::shared_ptr<const Key> key,
                                 const SerializationContext& serializationContext) const;

    /**
     * Scans all partitions (locking one partition at a time) to select and copy the top-K
     * candidate entries, requesting exactly 'K' candidates equal to the sort limit.
     */
    void computeTopKCandidates(const QueryStatsStore& queryStatsStore, const TopKSortSpec& spec);

    /**
     * Returns the document for the next top-K candidate, recording its key in '_topKConsumedKeys'
     * whether or not it materializes. Returns boost::none once the candidates are exhausted.
     */
    boost::optional<Document> nextTopKDocument(QueryStatsStore& queryStatsStore,
                                               const TopKSortSpec& spec);

    // Returns the next document of the full scan over every partition.
    GetNextResult nextFullScanDocument(QueryStatsStore& queryStatsStore);

    // The current partition copied from query stats store to avoid holding lock during reads.
    CopiedPartition _currentCopiedPartition;

    // The type of algorithm to use for transform identifiers as an enum, currently only
    // kHmacSha256
    // ("hmac-sha-256") is supported.
    const TransformAlgorithmEnum _algorithm;

    /**
     * Key used for SHA-256 HMAC application on field names.
     */
    std::string _hmacKey;

    // For-debug serialization of the corresponding 'DocumentSourceQueryStats' instance.
    BSONObj _serializedForDebug;

    // Top-K optimization set during optimization if the pattern is recognized.
    boost::optional<query_stats::TopKSortSpec> _topKSortSpec;

    // Candidate entries copied out of the store by the top-K scan, populated lazily on the first
    // doGetNext() call and popped from the back as they are materialized. Owning copies means
    // materialization never touches the store and cannot lose entries to eviction.
    boost::optional<std::vector<std::pair<std::size_t, QueryStatsEntry>>> _topKCandidateEntries;
    Date_t _topKScanTimestamp;

    // Keys of the top-K candidates already processed, whether they were emitted or failed in
    // 'toDocument'. If any candidate fails, we fall back to the full scan, skipping these keys.
    stdx::unordered_set<std::size_t> _topKConsumedKeys;
    bool _topKCandidateFailed{false};
};

}  // namespace exec::agg
}  // namespace mongo
