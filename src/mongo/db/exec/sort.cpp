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

#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

SortStage::SortStage(boost::intrusive_ptr<ExpressionContext> expCtx,
                     WorkingSet* ws,
                     SortPattern sortPattern,
                     uint64_t limit,
                     uint64_t maxMemoryUsageBytes,
                     std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType.rawData(), expCtx->opCtx),
      _ws(ws),
      _sortExecutor(std::move(sortPattern),
                    limit,
                    maxMemoryUsageBytes,
                    expCtx->tempDir,
                    expCtx->allowDiskUse) {
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
            auto member = _ws->get(id);
            invariant(member->metadata().hasSortKey());

            auto&& extractedMember = _ws->extract(id);

            try {
                auto sortKey = extractedMember.metadata().getSortKey();
                _sortExecutor.add(std::move(sortKey), std::move(extractedMember));
            } catch (const AssertionException&) {
                // Propagate runtime errors using the FAILED status code.
                *out = WorkingSetCommon::allocateStatusMember(_ws, exceptionToStatus());
                return PlanStage::FAILURE;
            }

            return PlanStage::NEED_TIME;
        } else if (code == PlanStage::IS_EOF) {
            // The child has returned all of its results. Record this fact so that subsequent calls
            // to 'doWork()' will perform sorting and unspool the sorted results.
            _populated = true;

            try {
                _sortExecutor.loadingDone();
            } catch (const AssertionException&) {
                // Propagate runtime errors using the FAILED status code.
                *out = WorkingSetCommon::allocateStatusMember(_ws, exceptionToStatus());
                return PlanStage::FAILURE;
            }

            return PlanStage::NEED_TIME;
        } else {
            *out = id;
        }

        return code;
    }

    auto nextWsm = _sortExecutor.getNextWsm();
    if (!nextWsm) {
        return PlanStage::IS_EOF;
    }

    *out = _ws->emplace(std::move(*nextWsm));
    return PlanStage::ADVANCED;
}

std::unique_ptr<PlanStageStats> SortStage::getStats() {
    _commonStats.isEOF = isEOF();
    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_SORT);
    ret->specific = _sortExecutor.cloneStats();
    ret->children.emplace_back(child()->getStats());
    return ret;
}

}  // namespace mongo
