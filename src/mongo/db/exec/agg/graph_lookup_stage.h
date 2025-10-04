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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lookup_set_cache.h"
#include "mongo/db/pipeline/spilling/spillable_deque.h"
#include "mongo/db/pipeline/spilling/spillable_map.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This class handles the execution part of the graph lookup aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceGraphLookUp, which handles the optimization part.
 */
class GraphLookUpStage final : public Stage {
public:
    GraphLookUpStage(StringData stageName,
                     const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                     GraphLookUpParams params,
                     boost::intrusive_ptr<ExpressionContext> fromExpCtx,
                     std::vector<BSONObj> fromPipeline,
                     boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwind,
                     Variables variables,
                     VariablesParseState variablesParseState);

    ~GraphLookUpStage() override;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    bool validateOperationContext(const OperationContext* opCtx) const final;

    bool usedDisk() const override {
        return _visitedDocuments.usedDisk() || _visitedFromValues.usedDisk() || _queue.usedDisk();
    }

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    void doForceSpill() final {
        spill(0);
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const final;

private:
    static constexpr StringData kFrontierValueField = "f"_sd;
    static constexpr StringData kDepthField = "d"_sd;

    GetNextResult doGetNext() final;

    void doDispose() final;

    void spill(int64_t maximumMemoryUsage);

    /**
     * If we have internalized a $unwind, getNext() dispatches to this function.
     */
    GetNextResult getNextUnwound();

    /**
     * Populates '_queue' with the '_startWith' value(s) from '_input' and then performs a
     * breadth-first search. Caller should check that _input is not boost::none.
     */
    void performSearch();

    void updateSpillingStats();

    void spillDuringVisitedUnwinding();

    /**
     * Perform a breadth-first search of the 'from' collection. '_queue' should already be
     * populated with the values for the initial query. Populates '_visited' with the result(s)
     * of the query.
     */
    void doBreadthFirstSearch();

    /**
     * Assert that '_visited' and '_queue' have not exceeded the maximum memory usage, and then
     * evict from '_cache' until this source is using less than 'maxMemoryUsageBytes'.
     */
    void checkMemoryUsage();

    /**
     * Updates '_cache' with 'result' appropriately, given that 'result' was retrieved when querying
     * for 'queried'.
     */
    void addToCache(const Document& result, const ValueFlatUnorderedSet& queried);

    /**
     * Process 'result', adding it to '_visited' with the given 'depth', and updating '_queue'
     * with the object's 'connectTo' values.
     */
    void addToVisitedAndQueue(Document result, long long depth);

    /**
     * Try to add given id to search queue. If id is already visited or is already in the queue,
     * will do nothing.
     */
    void addFromValueToQueueIfNeeded(Value id, long long depth);

    /**
     * Prepares the query to execute on the 'from' collection wrapped in a $match by using the
     * contents of '_queue'. Consumes from the _queue until it is empty or the match stage reached
     * BSONObjMaxUserSize.
     */
    struct Query {
        // Valid $match stage that we have to query, or boost::none if no query is needed.
        boost::optional<BSONObj> match;
        // Documents that are returned from in-memory cache.
        DocumentUnorderedSet cached;
        // Values from _queue that are processed by this query.
        ValueFlatUnorderedSet queried;
        // Depth of the documents, returned by the given query.
        long long depth;
    };
    Query makeQueryFromQueue();

    /**
     * Wraps frontier value and depth into a Document format for the _queue.
     */
    Document wrapFrontierValue(Value value, long long depth) const;

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedGraphLookupAllowed() const;

    /**
     * Create pipeline to get documents from the foreign collection.
     */
    std::unique_ptr<Pipeline> makePipeline(BSONObj match, bool allowForeignSharded);

    GraphLookUpParams _params;

    // The ExpressionContext used when performing aggregation pipelines against the '_from'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    // The aggregation pipeline to perform against the '_from' namespace.
    std::vector<BSONObj> _fromPipeline;

    // Keep track of a $unwind that was absorbed into this stage.
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> _unwind;
    boost::optional<SpillableDocumentMap::Iterator> _unwindIterator;

    // Holds variables defined both in this stage and in parent pipelines. These are copied to the
    // '_fromExpCtx' ExpressionContext's 'variables' and 'variablesParseState' for use in the
    // '_fromPipeline' execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    DocumentSourceGraphLookupStats _stats;

    // When we have internalized a $unwind, we must keep track of the input document, since we will
    // need it for multiple "getNext()" calls.
    boost::optional<Document> _input;

    // Tracks memory for _queue and _visited. _cache is allowed to use the remaining memory limit.
    MemoryUsageTracker _memoryUsageTracker;

    // Only used during the breadth-first search, tracks the set of values on the current frontier.
    // Contains documents with two fields: kFrontierValueField with a lookup value and kDepthField
    // with depth.
    SpillableDeque _queue;

    // Tracks nodes that have been discovered for a given input.
    // Contains visited documents by _id.
    SpillableDocumentMap _visitedDocuments;
    // Contains visited or already enqueued values of "connectFromField" to avoid duplicated
    // queries.
    SpillableValueSet _visitedFromValues;

    // Caches query results to avoid repeating any work. This structure is maintained across calls
    // to getNext().
    LookupSetCache _cache;

    // If we absorbed a $unwind that specified 'includeArrayIndex', this is used to populate that
    // field, tracking how many results we've returned so far for the current input document.
    long long _outputIndex = 0;
};
}  // namespace mongo::exec::agg
