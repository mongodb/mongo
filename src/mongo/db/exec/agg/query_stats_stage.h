// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/query/query_stats/transform_algorithm_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

using namespace query_stats;

namespace exec::agg {

class QueryStatsStage final : public Stage {
public:
    QueryStatsStage(std::string_view stageName,
                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    TransformAlgorithmEnum algorithm,
                    std::string hmacKey,
                    BSONObj serializedForDebug);

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

        bool empty() const;

        void load(QueryStatsStore& queryStatsStore);

        std::deque<QueryStatsEntry> statsEntries;

    private:
        Date_t _readTimestamp;
        QueryStatsStore::PartitionId _partitionId;
        bool _isLoaded{false};
    };

    GetNextResult doGetNext() final;

    boost::optional<Document> toDocument(const Date_t& partitionReadTime,
                                         const QueryStatsEntry& queryStatsEntry) const;


    BSONObj computeQueryStatsKey(std::shared_ptr<const Key> key,
                                 const SerializationContext& serializationContext) const;

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
};

}  // namespace exec::agg
}  // namespace mongo
