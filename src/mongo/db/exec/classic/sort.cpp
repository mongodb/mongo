/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/sort.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"

#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

SortStage::SortStage(boost::intrusive_ptr<ExpressionContext> expCtx,
                     WorkingSet* ws,
                     SortPattern sortPattern,
                     bool addSortKeyMetadata,
                     std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType.data(), expCtx.get()),
      _ws(ws),
      _sortKeyGen(sortPattern, expCtx->getCollator()),
      _addSortKeyMetadata(addSortKeyMetadata),
      _memoryTracker{OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(
          *expCtx, std::numeric_limits<int64_t>::max())} {
    _children.emplace_back(std::move(child));
}

PlanStage::StageState SortStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (!_populated) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        const StageState code = child()->work(&id);

        if (code == PlanStage::ADVANCED) {
            // The plan must be structured such that a previous stage has attached the sort key
            // metadata.
            spool(id);
            return PlanStage::NEED_TIME;
        } else if (code == PlanStage::IS_EOF) {
            // The child has returned all of its results. Record this fact so that subsequent calls
            // to 'doWork()' will perform sorting and unspool the sorted results.
            _populated = true;
            loadingDone();
            return PlanStage::NEED_TIME;
        } else {
            *out = id;
        }

        return code;
    }

    return unspool(out);
}

void SortStageDefault::loadingDone() {
    _sortExecutor.loadingDone();
}

void SortStageSimple::loadingDone() {
    _sortExecutor.loadingDone();
}

std::unique_ptr<PlanStageStats> SortStage::getStats() {
    _commonStats.isEOF = isEOF();
    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::unique_ptr<SpecificStats>{getSpecificStats()->clone()};
    ret->children.emplace_back(child()->getStats());
    return ret;
}

SortStageDefault::SortStageDefault(boost::intrusive_ptr<ExpressionContext> expCtx,
                                   WorkingSet* ws,
                                   SortPattern sortPattern,
                                   uint64_t limit,
                                   uint64_t maxMemoryUsageBytes,
                                   bool addSortKeyMetadata,
                                   std::unique_ptr<PlanStage> child)
    : SortStage(expCtx, ws, sortPattern, addSortKeyMetadata, std::move(child)),
      _sortExecutor(std::move(sortPattern),
                    limit,
                    maxMemoryUsageBytes,
                    expCtx->getTempDir(),
                    expCtx->getAllowDiskUse()) {}

void SortStageDefault::spool(WorkingSetID wsid) {
    SortableWorkingSetMember extractedMember{_ws->extract(wsid)};
    auto sortKey = _sortKeyGen.computeSortKey(*extractedMember);
    uint64_t prevMemBytes = _sortExecutor.stats().memoryUsageBytes;
    _sortExecutor.add(sortKey, extractedMember);
    _memoryTracker.add(_sortExecutor.stats().memoryUsageBytes - prevMemBytes);
}

PlanStage::StageState SortStageDefault::unspool(WorkingSetID* out) {
    if (!_sortExecutor.hasNext()) {
        // Storage is freed once all the documents have been returned.
        _memoryTracker.set(0);
        return PlanStage::IS_EOF;
    }

    auto&& [key, nextWsm] = _sortExecutor.getNext();
    *out = _ws->emplace(nextWsm.extract());

    if (_addSortKeyMetadata) {
        auto member = _ws->get(*out);
        member->metadata().setSortKey(std::move(key), _sortKeyGen.isSingleElementKey());
    }

    return PlanStage::ADVANCED;
}

SortStageSimple::SortStageSimple(boost::intrusive_ptr<ExpressionContext> expCtx,
                                 WorkingSet* ws,
                                 SortPattern sortPattern,
                                 uint64_t limit,
                                 uint64_t maxMemoryUsageBytes,
                                 bool addSortKeyMetadata,
                                 std::unique_ptr<PlanStage> child)
    : SortStage(expCtx, ws, sortPattern, addSortKeyMetadata, std::move(child)),
      _sortExecutor(std::move(sortPattern),
                    limit,
                    maxMemoryUsageBytes,
                    expCtx->getTempDir(),
                    expCtx->getAllowDiskUse()) {}

void SortStageSimple::spool(WorkingSetID wsid) {
    auto member = _ws->get(wsid);
    invariant(!member->metadata());
    invariant(!member->doc.value().metadata());
    invariant(member->hasObj());

    auto sortKey = _sortKeyGen.computeSortKeyFromDocument(member->doc.value());

    uint64_t prevMemBytes = _sortExecutor.stats().memoryUsageBytes;
    _sortExecutor.add(sortKey, member->doc.value().toBson());
    _memoryTracker.add(_sortExecutor.stats().memoryUsageBytes - prevMemBytes);
    _ws->free(wsid);
}

PlanStage::StageState SortStageSimple::unspool(WorkingSetID* out) {
    if (!_sortExecutor.hasNext()) {
        // Storage is freed once all the documents have been returned.
        _memoryTracker.set(0);
        return PlanStage::IS_EOF;
    }

    uint64_t prevMemBytes = _sortExecutor.stats().memoryUsageBytes;
    auto&& [key, nextObj] = _sortExecutor.getNext();
    _memoryTracker.add(_sortExecutor.stats().memoryUsageBytes - prevMemBytes);

    *out = _ws->allocate();
    auto member = _ws->get(*out);
    member->resetDocument(SnapshotId{}, nextObj.getOwned());
    member->transitionToOwnedObj();

    if (_addSortKeyMetadata) {
        member->metadata().setSortKey(std::move(key), _sortKeyGen.isSingleElementKey());
    }

    return PlanStage::ADVANCED;
}

}  // namespace mongo
