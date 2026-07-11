// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/granularity_rounder.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::exec::agg {

class BucketAutoStage final : public Stage {
public:
    BucketAutoStage(std::string_view stageName,
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

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const final;

    /**
     * TODO SERVER-112710: Remove '[[MONGO_MOD_PRIVATE]]' once document_source_bucket_auto_test.cpp
     * is split into two parts.
     */
    [[MONGO_MOD_PRIVATE]] const MemoryUsageTracker* getMemoryTracker_forTest() const {
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

    class Comparator {
    public:
        explicit Comparator(const ValueComparator& valueCmp) : _valueCmp(valueCmp) {}
        int operator()(const Value& lhs, const Value& rhs) const {
            return _valueCmp.compare(lhs, rhs);
        }

    private:
        ValueComparator _valueCmp;
    };

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

    // Pre-built context passed to every expression evaluation. tracker points to a sub-tracker of
    // _memoryTracker ("expressionEvaluation") when expression memory tracking is enabled, and is
    // null otherwise. Both fields are stable for the stage's lifetime.
    EvaluationContext _expressionEvalCtx;
};

}  // namespace mongo::exec::agg
