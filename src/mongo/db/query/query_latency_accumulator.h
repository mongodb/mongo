// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/util/duration.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Accumulates the total server-side execution time of a single logical query -- its originating
 * find/aggregate plus every getMore -- and records exactly one observation into the
 * per-plan-selection-strategy "queryLatencies" histogram when the query completes.
 *
 * Lives as a decoration on QueryLifespan, so one instance is shared by the originating command and
 * all of its getMores. The destructor reports the measurement exactly once: when the owning
 * cursor is exhausted, killed, or times out, or -- for a single-batch query -- at the end of the
 * originating operation.
 *
 * A query that does not run to completion still yields one observation, summing only the elapsed
 * time of the operations that completed (e.g. a killed getMore is not included).
 *
 * Reached via QueryLatencyAccumulator::get(opCtx) (which hides the QueryLifespan lookup).
 */
class [[MONGO_MOD_PUBLIC]] QueryLatencyAccumulator {
public:
    static QueryLatencyAccumulator& get(OperationContext* opCtx);

    QueryLatencyAccumulator() = default;
    ~QueryLatencyAccumulator();

    QueryLatencyAccumulator(const QueryLatencyAccumulator&) = delete;
    QueryLatencyAccumulator& operator=(const QueryLatencyAccumulator&) = delete;

    /**
     * Records the plan selection strategy for this query. Called from the originating
     * find/aggregate. A query that never records plan strategy -- e.g. a listCollections --
     * never emits an observation.
     */
    void recordStrategy(PlanSelectionStrategy strategy);

    /**
     * Adds one operation's execution time to the running total. No-ops until a strategy has been
     * recorded, or if this query has been excluded.
     */
    void addLatency(Microseconds elapsed);

    /**
     * Opts this query out of queryLatencies. Used for tailable / change-stream cursors, whose
     * unbounded lifetime would otherwise produce a misleading whole-lifetime observation on close.
     */
    void exclude();

private:
    boost::optional<PlanSelectionStrategy> _strategy;
    Microseconds _total{0};
    bool _excluded = false;
};

}  // namespace mongo
