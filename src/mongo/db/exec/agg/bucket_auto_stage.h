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
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter.h"

#include <memory>

namespace mongo::exec::agg {

class BucketAutoStage final : public Stage {
public:
    BucketAutoStage(StringData stageName,
                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    std::shared_ptr<std::vector<AccumulationStatement>> accumulatedFields,
                    std::shared_ptr<bool> populated,
                    boost::intrusive_ptr<Expression> groupByExpression,
                    boost::intrusive_ptr<GranularityRounder> granularityRounder,
                    int nBuckets);

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    bool usedDisk() const final {
        return _sorter ? _sorter->stats().spilledRanges() > 0
                       : _stats.spillingStats.getSpills() > 0;
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const final;

    const MemoryUsageTracker* getMemoryTracker_forTest() const {
        return &_memoryTracker;
    }


private:
    // struct for holding information about a bucket.
    struct Bucket {
        Bucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
               Value min,
               Value max,
               const std::vector<AccumulationStatement>& accumulationStatements);
        Value _min;
        Value _max;
        std::vector<boost::intrusive_ptr<AccumulatorState>> _accums;
    };


    struct BucketDetails {
        int currentBucketNum;
        long long approxBucketSize = 0;
        boost::optional<Value> previousMax;
        boost::optional<std::pair<Value, Document>> currentMin;
    };

    SortOptions makeSortOptions();

    /**
     * Consumes all of the documents from the source in the pipeline and sorts them by their
     * 'groupBy' value. This method might not be able to finish populating the sorter in a single
     * call if 'pSource' returns a DocumentSource::GetNextResult::kPauseExecution, so this returns
     * the last GetNextResult encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult populateSorter();

    void initializeBucketIteration();

    /**
     * Computes the 'groupBy' expression value for 'doc'.
     */
    Value extractKey(const Document& doc);

    /**
     * Returns the next bucket if exists. boost::none if none exist.
     */
    boost::optional<Bucket> populateNextBucket();

    boost::optional<std::pair<Value, Document>> adjustBoundariesAndGetMinForNextBucket(
        Bucket* currentBucket);

    /**
     * Adds the document in 'entry' to 'bucket' by updating the accumulators in 'bucket'.
     */
    void addDocumentToBucket(const std::pair<Value, Document>& entry, Bucket& bucket);

    /**
     * Makes a document using the information from bucket. This is what is returned when getNext()
     * is called.
     */
    Document makeDocument(const Bucket& bucket);

    GetNextResult doGetNext() final;
    void doDispose() final;
    void doForceSpill() final;

    SorterFileStats _sorterFileStats;
    std::unique_ptr<Sorter<Value, Document>> _sorter;
    std::unique_ptr<Sorter<Value, Document>::Iterator> _sortedInput;

    std::shared_ptr<std::vector<AccumulationStatement>> _accumulatedFields;

    // Per-field memory trackers corresponding to each AccumulationStatement in _accumulatedFields.
    // Caching these helps avoid lookups in the map in MemoryUsageTracker for every input document.
    std::vector<SimpleMemoryUsageTracker*> _accumulatedFieldMemoryTrackers;

    std::shared_ptr<bool> _populated;
    boost::intrusive_ptr<Expression> _groupByExpression;
    boost::intrusive_ptr<GranularityRounder> _granularityRounder;
    int _nBuckets;
    long long _nDocuments{0};
    long long _nDocPositions{0};
    BucketDetails _currentBucketDetails;

    DocumentSourceBucketAutoStats _stats;

    MemoryUsageTracker _memoryTracker;
};

}  // namespace mongo::exec::agg
