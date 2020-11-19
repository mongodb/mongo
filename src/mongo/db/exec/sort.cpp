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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/stats/resource_consumption_metrics.h"

namespace mongo {

SortStage::SortStage(boost::intrusive_ptr<ExpressionContext> expCtx,
                     WorkingSet* ws,
                     SortPattern sortPattern,
                     bool addSortKeyMetadata,
                     std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType.rawData(), expCtx.get()),
      _ws(ws),
      _sortKeyGen(sortPattern, expCtx->getCollator()),
      _addSortKeyMetadata(addSortKeyMetadata) {
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
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(expCtx()->opCtx);
    metricsCollector.incrementKeysSorted(_sortExecutor.stats().keysSorted);
    metricsCollector.incrementSorterSpills(_sortExecutor.stats().spills);
}

void SortStageSimple::loadingDone() {
    _sortExecutor.loadingDone();
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(expCtx()->opCtx);
    metricsCollector.incrementKeysSorted(_sortExecutor.stats().keysSorted);
    metricsCollector.incrementSorterSpills(_sortExecutor.stats().spills);
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
                    expCtx->tempDir,
                    expCtx->allowDiskUse) {}

void SortStageDefault::spool(WorkingSetID wsid) {
    SortableWorkingSetMember extractedMember{_ws->extract(wsid)};
    auto sortKey = _sortKeyGen.computeSortKey(*extractedMember);
    _sortExecutor.add(sortKey, extractedMember);
}

PlanStage::StageState SortStageDefault::unspool(WorkingSetID* out) {
    if (!_sortExecutor.hasNext()) {
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
                    expCtx->tempDir,
                    expCtx->allowDiskUse) {}

void SortStageSimple::spool(WorkingSetID wsid) {
    auto member = _ws->get(wsid);
    invariant(!member->metadata());
    invariant(!member->doc.value().metadata());
    invariant(member->hasObj());

    auto sortKey = _sortKeyGen.computeSortKeyFromDocument(member->doc.value());

    _sortExecutor.add(std::move(sortKey), member->doc.value().toBson());
    _ws->free(wsid);
}

PlanStage::StageState SortStageSimple::unspool(WorkingSetID* out) {
    if (!_sortExecutor.hasNext()) {
        return PlanStage::IS_EOF;
    }

    auto&& [key, nextObj] = _sortExecutor.getNext();

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
