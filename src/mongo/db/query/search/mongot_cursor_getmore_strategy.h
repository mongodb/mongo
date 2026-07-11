// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/executor/task_executor_cursor_options.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace executor {

/**
 * Defines the GetMore strategy for TaskExecutorCursor when configuring requests sent to mongot.
 */
class MongotTaskExecutorCursorGetMoreStrategy final : public TaskExecutorCursorGetMoreStrategy {
public:
    // Specifies the factor by which the batchSize sent from mongod to mongot increases per batch.
    static constexpr int kAlwaysPrefetchAfterNBatches = 3;

    MongotTaskExecutorCursorGetMoreStrategy(
        boost::optional<long long> startingBatchSize = mongot_cursor::kDefaultMongotBatchSize,
        DocsNeededBounds docsNeededBounds = DocsNeededBounds(docs_needed_bounds::Unknown(),
                                                             docs_needed_bounds::Unknown()),
        boost::optional<TenantId> tenantId = boost::none,
        std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
            searchIdLookupMetrics = nullptr);

    MongotTaskExecutorCursorGetMoreStrategy(MongotTaskExecutorCursorGetMoreStrategy&& other) =
        default;

    ~MongotTaskExecutorCursorGetMoreStrategy() final {}

    /**
     * Generates a BSONObj that represents the next getMore command to be dispatched to mongot.
     * CursorId and NamespaceString are included in the command object, and prevBatchNumReceived is
     * used to configure batchSize tuning.
     */
    BSONObj createGetMoreRequest(const CursorId& cursorId,
                                 const NamespaceString& nss,
                                 long long prevBatchNumReceived,
                                 long long totalNumReceived) final;

    /**
     * For the mongot cursor, we want to prefetch the next batch if we know we'll need another batch
     * (see _mustNeedAnotherBatch()), or if the maximum docsNeededBounds for this query is Unknown
     * and we've already received 3 batches.
     */
    bool shouldPrefetch(long long totalNumReceived, long long numBatchesReceived) const final;

    long long getCurrentBatchSize() const {
        tassert(8953003,
                "getCurrentBatchSize() should only be called when using the batchSize field.",
                _currentBatchSize.has_value());
        return *_currentBatchSize;
    }

    const std::vector<long long>& getBatchSizeHistory() const {
        return _batchSizeHistory;
    }

private:
    /**
     * Returns true if we will definitely need to request another batch from mongot in order to
     * fulfill this query (assuming the cursor is not closed / still has results to return).
     */
    bool _mustNeedAnotherBatch(long long totalNumReceived) const;

    /**
     * If batchSize tuning is not enabled, we'll see the initial batchSize for any getMore requests.
     * Otherwise, we'll apply tuning strategies to optimize batchSize of each batch requested.
     */
    long long _getNextBatchSize(long long prevBatchNumReceived);

    /**
     * Computes the next docsRequested value when the docsRequested option is enabled for mongot
     * requests.
     */
    boost::optional<long long> _getNextDocsRequested(long long totalNumReceived);

    // Set to boost::none if batchSize should not be set on getMore requests.
    boost::optional<long long> _currentBatchSize;

    // The min and max DocsNeededBounds that had been extracted from the user pipeline, to bound the
    // batchSize requested from mongot.
    DocsNeededBounds _docsNeededBounds;

    // Tracks all batchSizes sent to mongot, to be included in the $search explain execution stats.
    std::vector<long long> _batchSizeHistory;

    // The TenantId is necessary when retrieving the InternalSearchOptions cluster parameter value.
    boost::optional<TenantId> _tenantId;

    // These metrics are updated in the DocumentSourceInternalSearchIdLookUp and shared with this
    // class. We read the metrics in this class to to tune the batch size in some cases.
    std::shared_ptr<DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics>
        _searchIdLookupMetrics = nullptr;
};
}  // namespace executor
}  // namespace mongo
