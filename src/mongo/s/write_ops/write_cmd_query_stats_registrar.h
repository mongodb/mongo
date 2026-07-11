// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

#include <absl/container/flat_hash_set.h>

namespace mongo::query_stats {

// An insert command always has exactly one operation; this is its fixed QueryStatsMetrics index.
inline constexpr size_t kInsertOpIndex = 0;

// Matches update/delete op entry types, which track metrics per op index.
template <typename T>
concept HasIncludeQueryStatsMetricsForOpIndex =
    requires(T& e, boost::optional<int> i) { e.setIncludeQueryStatsMetricsForOpIndex(i); };

class WriteCmdQueryStatsRegistrar {
public:
    /**
     * The maximum num of write ops allowed to be requested for query stats metrics. This limit
     * prevents shard server responses from exceeding BSON object size limit due to the overhead
     * originating from CursorMetrics.
     */
    static constexpr size_t kMaxBatchOpsMetricsRequested = 10'000;

    WriteCmdQueryStatsRegistrar() {}

    /**
     * Entry point to register the write ops inside 'cmdRef' for query stats.
     *
     * @param skipRegistration - If true, parses the write ops in the command and computes the
     * QueryShapeHash, but doesn't register with the query stats store. Used for explains.
     */
    static void parseAndRegisterRequest(OperationContext* opCtx,
                                        WriteCommandRef cmdRef,
                                        bool skipRegistration = false);

    /**
     * Sets 'includeQueryStatsMetrics' in 'insertRequest' to request shard-side execution metrics
     * when the router has registered the insert for query stats collection.
     */
    void setIncludeQueryStatsMetricsIfRequested(OperationContext* opCtx,
                                                write_ops::InsertCommandRequest& insertRequest);

    /**
     * Sets 'includeQueryStatsMetricsForOpIndex' field in 'writeOpEntry' if the write op at
     * 'opIndex' is requested for query stats metrics. Does nothing if the op is not sampled or
     * the limit is reached. 'WriteOpEntry' must implement 'setIncludeQueryStatsMetricsForOpIndex'.
     *
     * Must be called at most once per opIndex per instance of the registrar.
     * '_numOpsWithMetricsRequested' is a counter; calling this multiple times for the same opIndex
     * would overcount and prematurely hit 'kMaxBatchOpsMetricsRequested'.
     */
    template <typename WriteOpEntry>
    requires(HasIncludeQueryStatsMetricsForOpIndex<WriteOpEntry>)
    void setIncludeQueryStatsMetricsForOpIndexIfRequested(OperationContext* opCtx,
                                                          int opIndex,
                                                          WriteOpEntry& writeOpEntry) {
        tassert(12808101, "opIndex must be >= 0", opIndex >= 0);
        CurOp* curOp = CurOp::get(opCtx);
        if (!shouldSetIncludeQueryStatsMetricsField(opCtx, curOp)) {
            return;
        }

        bool requestQueryStatsFromRemotes =
            query_stats::shouldRequestRemoteMetrics(curOp->debug(), opIndex);
        if (requestQueryStatsFromRemotes &&
            _numOpsWithMetricsRequested < kMaxBatchOpsMetricsRequested) {
            writeOpEntry.setIncludeQueryStatsMetricsForOpIndex(opIndex);
            _numOpsWithMetricsRequested++;
        } else {
            // Explicitly unset it to stop propagating the field and ignore it, in case the
            // field is set from user.
            writeOpEntry.setIncludeQueryStatsMetricsForOpIndex(boost::none);
        }
    }

private:
    /**
     * Returns true if this node is the original router that should stamp the
     * 'includeQueryStatsMetrics' field on outgoing shard commands. Returns false for aggregation
     * pipelines (which may issue writes internally via merge stages but are not user write
     * commands) and for commands forwarded from another router (where the field was already set by
     * the original router and should be left untouched).
     */
    static bool shouldSetIncludeQueryStatsMetricsField(OperationContext* opCtx, const CurOp* curOp);

    /**
     * Helper function to check if a command is an aggregation pipeline. This is useful because a
     * pipeline having a $merge stage may call cluster::write() to update documents and unexpectedly
     * enter the query stats registration for write commands.
     */
    static bool isAggregationPipeline(const CurOp* curOp);
    size_t _numOpsWithMetricsRequested = 0;
};
}  // namespace mongo::query_stats
