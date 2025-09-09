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
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort_executor.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

/**
 * This class handles the execution part of the sort aggregation stage and is part of the execution
 * pipeline. Its construction is based on DocumentSourceSort, which handles the optimization part.
 */
class SortStage final : public Stage {
public:
    SortStage(StringData stageName,
              const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
              const std::shared_ptr<SortExecutor<Document>>& sortExecutor,
              const std::shared_ptr<DocumentSourceSort::TimeSorterInterface>& timeSorter,
              const std::shared_ptr<SortKeyGenerator>& timeSorterPartitionKeyGen,
              bool outputSortKeyMetadata);

    bool usedDisk() const final;

    const SpecificStats* getSpecificStats() const final {
        return isBoundedSortStage() ? &_timeSorterStats : &_sortExecutor->stats();
    }

    bool isPopulated() {
        return _populated;
    };

    /**
     * Loads a document to be sorted. This can be used to sort a stream of documents that are not
     * coming from another DocumentSource. Once all documents have been added, the caller must call
     * loadingDone() before using getNext() to receive the documents in sorted order.
     */
    void loadDocument(Document&& doc);

    /**
     * Signals to the sort stage that there will be no more input documents. It is an error to call
     * loadDocument() once this method returns.
     */
    void loadingDone();

    /**
     * Returns true if the output documents of this $sort stage are supposed to have the sort key
     * metadata field populated.
     */
    bool shouldSetSortKeyMetadata() const {
        // TODO SERVER-98624 It would be preferable to just set '_outputSortKeyMetadata' based on
        // 'getNeedsMerge()' in the constructor or some earlier time. Sadly, we can't do this right
        // now without adding complexity elsewhere to account for mixed-version clusters. If you set
        // '_outputSortKeyMetadata' to true, then it will possibly mean serializing a new field when
        // sending a $sort to another node in the cluster (as of the time of this writing). This is
        // OK today because the callers who set this option during construction first must check the
        // FCV (and/or a feature flag), which guards against mixed-version scenarios.
        return _outputSortKeyMetadata || pExpCtx->getNeedsMerge();
    }

    bool isBoundedSortStage() const {
        return bool(_timeSorter);
    }

    const SimpleMemoryUsageTracker& getMemoryTracker_forTest() const {
        return _memoryTracker;
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const final;

private:
    GetNextResult doGetNext() final;

    void doForceSpill() final;
    /**
     * Gets the next document from the input. Caller must call timeSorterPeek() first, and it's
     * only valid to call timeSorterGetNext() if peek returned kAdvanced.
     */
    Document timeSorterGetNext();

    /**
     * Returns the time value used to sort 'doc', as well as the document that should be entered
     * into the sorter to eventually be returned. If we will need to later merge the sorted results
     * with other results, this method adds the full sort key as metadata onto 'doc' to speed up the
     * merge later.
     */
    std::pair<Date_t, Document> extractTime(Document&& doc) const;

    /**
     * Before returning anything, we have to consume all input and sort it. This method consumes all
     * input and prepares the sorted stream '_output'.
     *
     * This method may not be able to finish populating the sorter in a single call if 'pSource'
     * returns a DocumentSource::GetNextResult::kPauseExecution, so it returns the last
     * GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populate();

    GetNextResult::ReturnStatus timeSorterPeek();

    /**
     * Populates this stage specific stats using data from _timeSorter. Should be called atleast
     * once after _timeSorter is exhausted. Can be called before to provide "online" stats during
     * cursor lifetime.
     */
    void updateTimeSorterStats();

    /**
     * Peeks at the next document in the input, but ignores documents whose partition key differs
     * from the current partition key (if there is one).
     */
    GetNextResult::ReturnStatus timeSorterPeekSamePartition();

    /**
     * Returns the sort key for 'doc', as well as the document that should be entered into the
     * sorter to eventually be returned. If we will need to later merge the sorted results with
     * other results, this method adds the sort key as metadata onto 'doc' to speed up the merge
     * later.
     */
    std::pair<Value, Document> extractSortKey(Document&& doc) const;

    std::shared_ptr<SortExecutor<Document>> _sortExecutor;
    std::shared_ptr<DocumentSourceSort::TimeSorterInterface> _timeSorter;
    SimpleMemoryUsageTracker _memoryTracker;

    std::shared_ptr<SortKeyGenerator> _timeSorterPartitionKeyGen;
    boost::optional<SortKeyGenerator> _sortKeyGen;

    // The next document that will be returned by timeSorterGetNext().
    // timeSorterPeek() fills it in, and timeSorterGetNext() empties it.
    boost::optional<Document> _timeSorterNextDoc;

    // The current partition key.
    // If _timeSorterNextDoc has a document then this represents the partition key of
    // that document.
    // If _timeSorterNextDoc is empty then this represents the partition key of
    // the document last returned by timeSorterGetNext().
    boost::optional<Value> _timeSorterCurrentPartition;

    // Used only if _timeSorter is present.
    SortStats _timeSorterStats;

    // Used in timeSorterPeek() to avoid calling getNext() on an exhausted pSource.
    bool _timeSorterInputEOF = false;

    bool _populated = false;

    // Whether to include metadata including the sort key in the output documents from this stage.
    bool _outputSortKeyMetadata = false;
};
}  // namespace agg
}  // namespace exec
}  // namespace mongo
