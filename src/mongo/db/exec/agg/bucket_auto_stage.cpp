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

#include "mongo/db/exec/agg/bucket_auto_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/accumulator_for_bucket_auto.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceBucketAutoToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* bucketAutoDS = dynamic_cast<DocumentSourceBucketAuto*>(documentSource.get());

    tassert(10980200, "expected 'DocumentSourceBucketAuto' type", bucketAutoDS);

    return make_intrusive<exec::agg::BucketAutoStage>(bucketAutoDS->kStageName,
                                                      bucketAutoDS->getExpCtx(),
                                                      bucketAutoDS->_accumulatedFields,
                                                      bucketAutoDS->_populated,
                                                      bucketAutoDS->_groupByExpression,
                                                      bucketAutoDS->_granularityRounder,
                                                      bucketAutoDS->_nBuckets);
}

REGISTER_AGG_STAGE_MAPPING(bucketAuto,
                           DocumentSourceBucketAuto::id,
                           documentSourceBucketAutoToStageFn);
namespace exec::agg {

BucketAutoStage::BucketAutoStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<std::vector<AccumulationStatement>> accumulatedFields,
    std::shared_ptr<bool> populated,
    boost::intrusive_ptr<Expression> groupByExpression,
    boost::intrusive_ptr<GranularityRounder> granularityRounder,
    int nBuckets)
    : exec::agg::Stage(stageName, expCtx),
      _sorterFileStats(nullptr /*sorterTracker*/),
      _accumulatedFields(std::move(accumulatedFields)),
      _populated(std::move(populated)),
      _groupByExpression(std::move(groupByExpression)),
      _granularityRounder(std::move(granularityRounder)),
      _nBuckets{nBuckets},
      _currentBucketDetails{0},
      _memoryTracker{OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
          *pExpCtx,
          pExpCtx->getAllowDiskUse() && !pExpCtx->getInRouter(),
          loadMemoryLimit(StageMemoryLimit::DocumentSourceBucketAutoMaxMemoryBytes))} {
    for (auto&& accumulationStatement : *_accumulatedFields) {
        _accumulatedFieldMemoryTrackers.push_back(&_memoryTracker[accumulationStatement.fieldName]);
    }
}

SortOptions BucketAutoStage::makeSortOptions() {
    SortOptions opts;
    opts.MaxMemoryUsageBytes(_memoryTracker.maxAllowedMemoryUsageBytes());
    if (pExpCtx->getAllowDiskUse() && !pExpCtx->getInRouter()) {
        opts.TempDir(pExpCtx->getTempDir());
        opts.FileStats(&_sorterFileStats);
    }
    return opts;
}

GetNextResult BucketAutoStage::populateSorter() {
    if (!_sorter) {
        const auto& valueCmp = pExpCtx->getValueComparator();
        auto comparator = [valueCmp](const Value& lhs, const Value& rhs) {
            return valueCmp.compare(lhs, rhs);
        };

        _sorter = Sorter<Value, Document>::make(makeSortOptions(), comparator);
    }

    auto next = pSource->getNext();
    auto prevMemUsage = _sorter->stats().memUsage();
    for (; next.isAdvanced(); next = pSource->getNext()) {
        auto nextDoc = next.releaseDocument();
        auto key = extractKey(nextDoc);

        auto doc = Document{{AccumulatorN::kFieldNameOutput, Value(std::move(nextDoc))},
                            {AccumulatorN::kFieldNameGeneratedSortKey, Value(_nDocPositions++)}};
        _sorter->add(std::move(key), std::move(doc));

        // To provide as accurate memory accounting as possible, we update the memory tracker after
        // we add each document to the sorter.
        auto currMemUsage = _sorter->stats().memUsage();
        _memoryTracker.add(currMemUsage - prevMemUsage);
        prevMemUsage = currMemUsage;

        ++_nDocuments;
    }

    return next;
}

Value BucketAutoStage::extractKey(const Document& doc) {
    if (!_groupByExpression) {
        return Value(BSONNULL);
    }

    Value key = _groupByExpression->evaluate(doc, &pExpCtx->variables);

    if (_granularityRounder) {
        uassert(40258,
                str::stream() << "$bucketAuto can specify a 'granularity' with numeric boundaries "
                                 "only, but found a value with type: "
                              << typeName(key.getType()),
                key.numeric());

        double keyValue = key.coerceToDouble();
        uassert(
            40259,
            "$bucketAuto can specify a 'granularity' with numeric boundaries only, but found a NaN",
            !std::isnan(keyValue));

        uassert(40260,
                "$bucketAuto can specify a 'granularity' with non-negative numbers only, but found "
                "a negative number",
                keyValue >= 0.0);
    }

    // To be consistent with the $group stage, we consider "missing" to be equivalent to null when
    // grouping values into buckets.
    return key.missing() ? Value(BSONNULL) : std::move(key);
}

void BucketAutoStage::addDocumentToBucket(const std::pair<Value, Document>& entry, Bucket& bucket) {
    invariant(pExpCtx->getValueComparator().evaluate(entry.first >= bucket._max));
    bucket._max = entry.first;

    const size_t numAccumulators = _accumulatedFields->size();
    for (size_t k = 0; k < numAccumulators; k++) {
        AccumulatorState& accumulator = *bucket._accums[k];
        if (accumulator.needsInput()) {
            bool isPositionalAccum = isPositionalAccumulator(accumulator.getOpName());
            auto value = entry.second.getField(AccumulatorN::kFieldNameOutput);
            auto evaluated = (*_accumulatedFields)[k].expr.argument->evaluate(value.getDocument(),
                                                                              &pExpCtx->variables);

            auto prevMemUsage = accumulator.getMemUsage();
            if (isPositionalAccum) {
                auto wrapped = Value(Document{
                    {AccumulatorN::kFieldNameGeneratedSortKey,
                     entry.second.getField(AccumulatorN::kFieldNameGeneratedSortKey)},
                    {AccumulatorN::kFieldNameOutput, std::move(evaluated)},
                });
                accumulator.process(Value(std::move(wrapped)), false);
            } else {
                accumulator.process(std::move(evaluated), false);
            }
            _accumulatedFieldMemoryTrackers[k]->add(accumulator.getMemUsage() - prevMemUsage);
        }
    }
}

boost::optional<std::pair<Value, Document>> BucketAutoStage::adjustBoundariesAndGetMinForNextBucket(
    Bucket* currentBucket) {
    auto getNextValIfPresent = [this]() {
        return _sortedInput->more()
            ? boost::optional<std::pair<Value, Document>>(_sortedInput->next())
            : boost::none;
    };

    auto nextValue = getNextValIfPresent();
    if (_granularityRounder) {
        Value boundaryValue = _granularityRounder->roundUp(currentBucket->_max);

        // If there are any values that now fall into this bucket after we round the
        // boundary, absorb them into this bucket too.
        while (nextValue &&
               (pExpCtx->getValueComparator().evaluate(boundaryValue > nextValue->first) ||
                pExpCtx->getValueComparator().evaluate(currentBucket->_max == nextValue->first))) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // Handle the special case where the largest value in the first bucket is zero. In this
        // case, we take the minimum boundary of the next bucket and round it down. We then set the
        // maximum boundary of the current bucket to be the rounded down value. This maintains that
        // the maximum boundary of the current bucket is exclusive and the minimum boundary of the
        // next bucket is inclusive.
        double currentMax = boundaryValue.coerceToDouble();
        if (currentMax == 0.0 && nextValue) {
            currentBucket->_max = _granularityRounder->roundDown(nextValue->first);
        } else {
            currentBucket->_max = boundaryValue;
        }
    } else {
        // If there are any more values that are equal to the boundary value, then absorb
        // them into the current bucket too.
        while (nextValue &&
               pExpCtx->getValueComparator().evaluate(currentBucket->_max == nextValue->first)) {
            addDocumentToBucket(*nextValue, *currentBucket);
            nextValue = getNextValIfPresent();
        }

        // If there is a bucket that comes after the current bucket, then the current bucket's max
        // boundary is updated to the next bucket's min. This makes it so that buckets' min
        // boundaries are inclusive and max boundaries are exclusive (except for the last bucket,
        // which has an inclusive max).
        if (nextValue) {
            currentBucket->_max = nextValue->first;
        }
    }
    return nextValue;
}

boost::optional<BucketAutoStage::Bucket> BucketAutoStage::populateNextBucket() {
    // If there was a bucket before this, the 'currentMin' should be populated, or there are no more
    // documents.
    if (!_currentBucketDetails.currentMin && !_sortedInput->more()) {
        return {};
    }

    std::pair<Value, Document> currentValue =
        _currentBucketDetails.currentMin ? *_currentBucketDetails.currentMin : _sortedInput->next();

    Bucket currentBucket(pExpCtx, currentValue.first, currentValue.first, *_accumulatedFields);

    // If we have a granularity specified and if there is a bucket that came before the current
    // bucket being added, then the current bucket's min boundary is updated to be the previous
    // bucket's max boundary. This makes it so that bucket boundaries follow the granularity, have
    // inclusive minimums, and have exclusive maximums.
    if (_granularityRounder) {
        currentBucket._min = _currentBucketDetails.previousMax.value_or(
            _granularityRounder->roundDown(currentValue.first));
    }

    // Evaluate each initializer against an empty document. Normally the initializer can refer to
    // the group key, but in $bucketAuto there is no single group key per bucket.
    Document emptyDoc;
    for (size_t k = 0; k < _accumulatedFields->size(); ++k) {
        Value initializerValue =
            (*_accumulatedFields)[k].expr.initializer->evaluate(emptyDoc, &pExpCtx->variables);
        AccumulatorState& accumulator = *currentBucket._accums[k];
        accumulator.startNewGroup(initializerValue);
        _accumulatedFieldMemoryTrackers[k]->add(accumulator.getMemUsage());
    }

    // Add 'approxBucketSize' number of documents to the current bucket. If this is the last bucket,
    // add all the remaining documents.
    addDocumentToBucket(currentValue, currentBucket);
    const auto isLastBucket = (_currentBucketDetails.currentBucketNum == _nBuckets);
    for (long long i = 1;
         _sortedInput->more() && (i < _currentBucketDetails.approxBucketSize || isLastBucket);
         i++) {
        addDocumentToBucket(_sortedInput->next(), currentBucket);
    }

    // Modify the bucket details for next bucket.
    _currentBucketDetails.currentMin = adjustBoundariesAndGetMinForNextBucket(&currentBucket);
    _currentBucketDetails.previousMax = currentBucket._max;

    // Free the accumulators' memory usage that we've tallied from this past bucket.
    for (size_t i = 0; i < currentBucket._accums.size(); i++) {
        // Subtract the current usage.
        _accumulatedFieldMemoryTrackers[i]->add(-1 * currentBucket._accums[i]->getMemUsage());

        currentBucket._accums[i]->reduceMemoryConsumptionIfAble();

        // Update the memory usage for this AccumulationStatement.
        _accumulatedFieldMemoryTrackers[i]->add(currentBucket._accums[i]->getMemUsage());
    }
    return currentBucket;
}

void BucketAutoStage::initializeBucketIteration() {
    // Initialize the iterator on '_sorter'.
    invariant(_sorter);
    _sortedInput = _sorter->done();

    _stats.spillingStats.updateSpillingStats(_sorter->stats().spilledRanges(),
                                             _sorterFileStats.bytesSpilledUncompressed(),
                                             _sorter->stats().spilledKeyValuePairs(),
                                             _sorterFileStats.bytesSpilled());
    bucketAutoCounters.incrementPerSpilling(_sorter->stats().spilledRanges(),
                                            _sorterFileStats.bytesSpilledUncompressed(),
                                            _sorter->stats().spilledKeyValuePairs(),
                                            _sorterFileStats.bytesSpilled());

    _sorter.reset();

    // If there are no buckets, then we don't need to populate anything.
    if (_nBuckets == 0) {
        return;
    }

    // Calculate the approximate bucket size. We attempt to fill each bucket with this many
    // documents.
    _currentBucketDetails.approxBucketSize = std::round(double(_nDocuments) / double(_nBuckets));

    if (_currentBucketDetails.approxBucketSize < 1) {
        // If the number of buckets is larger than the number of documents, then we try to make
        // as many buckets as possible by placing each document in its own bucket.
        _currentBucketDetails.approxBucketSize = 1;
    }
}

Document BucketAutoStage::makeDocument(const Bucket& bucket) {
    const size_t nAccumulatedFields = _accumulatedFields->size();
    MutableDocument out(1 + nAccumulatedFields);

    out.addField("_id", Value{Document{{"min", bucket._min}, {"max", bucket._max}}});

    const bool mergingOutput = false;
    for (size_t i = 0; i < nAccumulatedFields; i++) {
        Value val = bucket._accums[i]->getValue(mergingOutput);

        // To be consistent with the $group stage, we consider "missing" to be equivalent to null
        // when evaluating accumulators.
        out.addField((*_accumulatedFields)[i].fieldName,
                     val.missing() ? Value(BSONNULL) : std::move(val));
    }
    return out.freeze();
}

BucketAutoStage::Bucket::Bucket(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                Value min,
                                Value max,
                                const std::vector<AccumulationStatement>& accumulationStatements)
    : _min(min), _max(max) {
    _accums.reserve(accumulationStatements.size());
    for (auto&& accumulationStatement : accumulationStatements) {
        _accums.push_back(accumulationStatement.makeAccumulator());
    }
}

void BucketAutoStage::doDispose() {
    _sortedInput.reset();

    _memoryTracker.resetCurrent();
}

GetNextResult BucketAutoStage::doGetNext() {
    if (!*_populated) {
        auto populationResult = populateSorter();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());

        initializeBucketIteration();
        *_populated = true;
    }

    if (!_sortedInput) {
        // We have been disposed. Return EOF.
        _memoryTracker.resetCurrent();
        return GetNextResult::makeEOF();
    }

    if (_currentBucketDetails.currentBucketNum++ < _nBuckets) {
        if (auto bucket = populateNextBucket()) {
            return makeDocument(*bucket);
        }
    }
    dispose();
    return GetNextResult::makeEOF();
}

void BucketAutoStage::doForceSpill() {
    if (_sorter) {
        auto prevSorterSize = _sorter->stats().memUsage();
        _sorter->spill();

        // The sorter's size has decreased after spilling. Subtract this freed memory from the
        // memory tracker.
        auto currSorterSize = _sorter->stats().memUsage();
        _memoryTracker.add(-1 * (prevSorterSize - currSorterSize));
    } else if (_sortedInput && _sortedInput->spillable()) {
        SortOptions opts = makeSortOptions();
        SorterTracker tracker;
        opts.sorterTracker = &tracker;

        auto previousSpilledBytes = _sorterFileStats.bytesSpilledUncompressed();

        _sortedInput = _sortedInput->spill(opts, Sorter<Value, Document>::Settings{});

        auto spilledDataStorageIncrease = _stats.spillingStats.updateSpillingStats(
            1,
            _sorterFileStats.bytesSpilledUncompressed() - previousSpilledBytes,
            tracker.spilledKeyValuePairs.loadRelaxed(),
            _sorterFileStats.bytesSpilled());
        bucketAutoCounters.incrementPerSpilling(1,
                                                _sorterFileStats.bytesSpilledUncompressed() -
                                                    previousSpilledBytes,
                                                tracker.spilledKeyValuePairs.loadRelaxed(),
                                                spilledDataStorageIncrease);
        // The sorter iterator has spilled everything. Set the memory tracker and the
        // accumulators' trackers back to 0.
        _memoryTracker.resetCurrent();
    }
}

Document BucketAutoStage::getExplainOutput(const SerializationOptions& opts) const {
    MutableDocument out(Stage::getExplainOutput(opts));
    out["usedDisk"] = opts.serializeLiteral(_stats.spillingStats.getSpills() > 0);
    out["spills"] = opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpills()));
    out["spilledDataStorageSize"] = opts.serializeLiteral(
        static_cast<long long>(_stats.spillingStats.getSpilledDataStorageSize()));
    out["spilledBytes"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledBytes()));
    out["spilledRecords"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledRecords()));
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        out["peakTrackedMemBytes"] =
            opts.serializeLiteral(static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes()));
    }


    return out.freeze();
}
}  // namespace exec::agg
}  // namespace mongo
