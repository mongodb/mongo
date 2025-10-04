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

#include "mongo/db/exec/agg/sort_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSortToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto sortDS = boost::dynamic_pointer_cast<DocumentSourceSort>(documentSource);

    tassert(10423000, "expected 'DocumentSourceSort' type", sortDS);

    return make_intrusive<exec::agg::SortStage>(sortDS->kStageName,
                                                sortDS->getExpCtx(),
                                                sortDS->_sortExecutor,
                                                sortDS->_timeSorter,
                                                sortDS->_timeSorterPartitionKeyGen,
                                                sortDS->_outputSortKeyMetadata);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(sort, DocumentSourceSort::id, documentSourceSortToStageFn)

[[noreturn]] void throwCannotHandleControlEvent() {
    tasserted(10358905, "sort does not support control events");
}

SortStage::SortStage(StringData stageName,
                     const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                     const std::shared_ptr<SortExecutor<Document>>& sortExecutor,
                     const std::shared_ptr<DocumentSourceSort::TimeSorterInterface>& timeSorter,
                     const std::shared_ptr<SortKeyGenerator>& timeSorterPartitionKeyGen,
                     bool outputSortKeyMetadata)
    : Stage(stageName, pExpCtx),
      _sortExecutor(sortExecutor),
      _timeSorter(timeSorter),
      _memoryTracker{OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(
          *pExpCtx, sortExecutor->getMaxMemoryBytes())},
      _timeSorterPartitionKeyGen(timeSorterPartitionKeyGen),
      // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
      // by a metadata field.
      _sortKeyGen({sortExecutor->sortPattern(), pExpCtx->getCollator()}),
      _outputSortKeyMetadata(outputSortKeyMetadata) {
    if (_timeSorter) {
        _timeSorterStats = _sortExecutor->stats();
    }
};

bool SortStage::usedDisk() const {
    return isBoundedSortStage() ? _timeSorter->stats().spilledRanges() > 0
                                : _sortExecutor->wasDiskUsed();
}

Document SortStage::getExplainOutput(const SerializationOptions& opts) const {
    // TODO SERVER-108419: Move all execution stats.
    MutableDocument mutDoc(Stage::getExplainOutput(opts));
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        mutDoc["peakTrackedMemBytes"] =
            opts.serializeLiteral(static_cast<long long>(_memoryTracker.peakTrackedMemoryBytes()));
    }
    return mutDoc.freeze();
}

GetNextResult SortStage::doGetNext() {
    if (_timeSorter) {
        // If the _timeSorter is exhausted but we have more input, it must be because we just
        // finished a partition. Restart the _timeSorter to make it ready for the next partition.
        if (_timeSorter->getState() == DocumentSourceSort::TimeSorterInterface::State::kDone &&
            timeSorterPeek() == GetNextResult::ReturnStatus::kAdvanced) {
            updateTimeSorterStats();
            _timeSorter->restart();
            _timeSorterCurrentPartition.reset();
        }

        auto prevMemUsage = _timeSorter->stats().memUsage();

        // Only pull input as necessary to get _timeSorter to have a result.
        while (_timeSorter->getState() == DocumentSourceSort::TimeSorterInterface::State::kWait) {
            auto status = timeSorterPeekSamePartition();
            switch (status) {
                case GetNextResult::ReturnStatus::kPauseExecution: {
                    return GetNextResult::makePauseExecution();
                }
                case GetNextResult::ReturnStatus::kEOF: {
                    // We've reached the end of the current partition. Tell _timeSorter there will
                    // be no more input. In response, its state will never be kWait again unless we
                    // restart it, so we can proceed to drain all the documents currently held by
                    // the sorter.
                    _timeSorter->done();
                    tassert(6434802,
                            "SortStage::_timeSorter waiting for input that will not arrive",
                            _timeSorter->getState() !=
                                DocumentSourceSort::TimeSorterInterface::State::kWait);
                    continue;
                }
                case GetNextResult::ReturnStatus::kAdvanced: {
                    auto [time, doc] = extractTime(timeSorterGetNext());
                    _timeSorter->add({time}, doc);
                    auto currMemUsage = _timeSorter->stats().memUsage();
                    _memoryTracker.add(currMemUsage - prevMemUsage);
                    prevMemUsage = currMemUsage;
                    continue;
                }
                case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
                    throwCannotHandleControlEvent();
                }
            }
        }

        if (_timeSorter->getState() == DocumentSourceSort::TimeSorterInterface::State::kDone) {
            updateTimeSorterStats();
            return GetNextResult::makeEOF();
        }

        // In the bounded sort case, memory is released incrementally as documents are returned via
        // next(). The memory tracker is updated to reflect these changes.
        prevMemUsage = _timeSorter->stats().memUsage();
        Document nextDocument = _timeSorter->next().second;
        auto currMemUsage = _timeSorter->stats().memUsage();
        _memoryTracker.add(currMemUsage - prevMemUsage);
        return nextDocument;
    }

    if (!_populated) {
        auto populationResult = populate();
        if (MONGO_unlikely(populationResult.isAdvancedControlDocument())) {
            throwCannotHandleControlEvent();
        }
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());
    }

    if (!_sortExecutor->hasNext()) {
        _memoryTracker.set(0);
        return GetNextResult::makeEOF();
    }

    // In the general case (without _timeSorter), memory is not released incrementally while
    // returning sorted results. Memory is ultimately cleared in bulk once EOF is reached via
    // _sortExecutor's call to clearSortTable() during hasNext().
    return GetNextResult{_sortExecutor->getNext().second};
}

void SortStage::doForceSpill() {
    if (_sortExecutor) {
        auto prevSorterSize = _sortExecutor->stats().memoryUsageBytes;
        _sortExecutor->forceSpill();
        auto currSorterSize = _sortExecutor->stats().memoryUsageBytes;
        _memoryTracker.add(currSorterSize - prevSorterSize);
    }
    if (_timeSorter) {
        auto prevSorterSize = _timeSorter->stats().memUsage();
        _timeSorter->forceSpill();
        auto currSorterSize = _timeSorter->stats().memUsage();
        _memoryTracker.add(currSorterSize - prevSorterSize);
    }
}

GetNextResult::ReturnStatus SortStage::timeSorterPeek() {
    if (_timeSorterNextDoc) {
        return GetNextResult::ReturnStatus::kAdvanced;
    }
    if (_timeSorterInputEOF) {
        return GetNextResult::ReturnStatus::kEOF;
    }

    auto next = pSource->getNext();
    auto status = next.getStatus();
    switch (status) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            _timeSorterNextDoc = next.getDocument();
            return status;
        }
        case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            throwCannotHandleControlEvent();
        }
        case GetNextResult::ReturnStatus::kEOF:
            _timeSorterInputEOF = true;
            return status;
        case GetNextResult::ReturnStatus::kPauseExecution:
            return status;
    }
    MONGO_UNREACHABLE_TASSERT(6434800);
}

void SortStage::updateTimeSorterStats() {
    tassert(10321900,
            "Called updateTimeSorterStats() on a non-bounded sort stage",
            isBoundedSortStage());
    _timeSorterStats.totalDataSizeBytes = _timeSorter->stats().bytesSorted();
    _timeSorterStats.memoryUsageBytes = _timeSorter->stats().memUsage();
    _timeSorterStats.keysSorted = _timeSorter->stats().numSorted();
    auto sorterFileStats = _sortExecutor->getSorterFileStats();
    if (sorterFileStats) {
        _timeSorterStats.spillingStats.setSpills(_timeSorter->stats().spilledRanges());
        _timeSorterStats.spillingStats.setSpilledRecords(
            _timeSorter->stats().spilledKeyValuePairs());
        _timeSorterStats.spillingStats.setSpilledBytes(sorterFileStats->bytesSpilledUncompressed());
        _timeSorterStats.spillingStats.updateSpilledDataStorageSize(
            sorterFileStats->bytesSpilled());
    }
}

GetNextResult::ReturnStatus SortStage::timeSorterPeekSamePartition() {
    auto status = timeSorterPeek();
    switch (status) {
        case GetNextResult::ReturnStatus::kEOF:
        case GetNextResult::ReturnStatus::kPauseExecution:
            return status;
        case GetNextResult::ReturnStatus::kAdvanced:
            break;
        case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            throwCannotHandleControlEvent();
        }
    }

    if (!_timeSorterPartitionKeyGen) {
        // No partition key means everything is in the same partition.
        return GetNextResult::ReturnStatus::kAdvanced;
    } else {
        auto prevPartition = std::move(_timeSorterCurrentPartition);
        _timeSorterCurrentPartition =
            _timeSorterPartitionKeyGen->computeSortKeyFromDocument(*_timeSorterNextDoc);

        if (!prevPartition) {
            // No previous partition means there is no constraint.
            return GetNextResult::ReturnStatus::kAdvanced;
        } else if (pExpCtx->getValueComparator().evaluate(*_timeSorterCurrentPartition ==
                                                          *prevPartition)) {
            // Next is in the same partition.
            return GetNextResult::ReturnStatus::kAdvanced;
        } else {
            // Next is in a new partition: pretend we don't have a next document.
            return GetNextResult::ReturnStatus::kEOF;
        }
    }
}

GetNextResult SortStage::populate() {
    auto nextInput = pSource->getNext();
    auto prevMemUsage = _sortExecutor->stats().memoryUsageBytes;
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        loadDocument(nextInput.releaseDocument());
        auto currMemUsage = _sortExecutor->stats().memoryUsageBytes;
        _memoryTracker.add(currMemUsage - prevMemUsage);
        prevMemUsage = currMemUsage;
    }
    if (nextInput.isEOF()) {
        loadingDone();
    }
    return nextInput;
}

void SortStage::loadDocument(Document&& doc) {
    invariant(!_populated);

    Value sortKey;
    Document docForSorter;
    // We always need to extract the sort key if we've reached this point. If the query system had
    // already computed the sort key we'd have split the pipeline there, would be merging presorted
    // documents, and wouldn't use this method.
    std::tie(sortKey, docForSorter) = extractSortKey(std::move(doc));
    _sortExecutor->add(sortKey, docForSorter);
}

void SortStage::loadingDone() {
    _sortExecutor->loadingDone();
    _populated = true;
}

std::pair<Date_t, Document> SortStage::extractTime(Document&& doc) const {
    const auto& fullPath = _sortExecutor->sortPattern().back().fieldPath->fullPath();
    auto time = doc.getField(StringData{fullPath});
    uassert(6369909,
            "$_internalBoundedSort only handles BSONType::date values",
            time.getType() == BSONType::date);
    auto date = time.getDate();

    if (shouldSetSortKeyMetadata()) {
        // If this sort stage is part of a merged pipeline, make sure that each Document's sort key
        // gets saved with its metadata.
        Value sortKey = _sortKeyGen->computeSortKeyFromDocument(doc);
        MutableDocument toBeSorted(std::move(doc));
        toBeSorted.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());

        return std::make_pair(date, toBeSorted.freeze());
    }

    return std::make_pair(date, std::move(doc));
}

Document SortStage::timeSorterGetNext() {
    tassert(6434801,
            "timeSorterGetNext() is only valid after timeSorterPeek() returns isAdvanced()",
            _timeSorterNextDoc);
    auto result = std::move(*_timeSorterNextDoc);
    _timeSorterNextDoc.reset();
    return result;
}

std::pair<Value, Document> SortStage::extractSortKey(Document&& doc) const {
    Value sortKey = _sortKeyGen->computeSortKeyFromDocument(doc);

    if (shouldSetSortKeyMetadata()) {
        // If this sort stage is part of a merged pipeline, make sure that each Document's sort key
        // gets saved with its metadata.
        MutableDocument toBeSorted(std::move(doc));
        toBeSorted.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());

        return std::make_pair(std::move(sortKey), toBeSorted.freeze());
    } else {
        return std::make_pair(std::move(sortKey), std::move(doc));
    }
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
