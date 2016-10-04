/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/ensure_sorted.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/query/find_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using stdx::make_unique;

const char* EnsureSortedStage::kStageType = "ENSURE_SORTED";

EnsureSortedStage::EnsureSortedStage(OperationContext* opCtx,
                                     BSONObj pattern,
                                     WorkingSet* ws,
                                     PlanStage* child)
    : PlanStage(kStageType, opCtx), _ws(ws) {
    _children.emplace_back(child);
    _pattern = FindCommon::transformSortSpec(pattern);
}

bool EnsureSortedStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState EnsureSortedStage::doWork(WorkingSetID* out) {
    StageState stageState = child()->work(out);

    if (PlanStage::ADVANCED == stageState) {
        // We extract the sort key from the WSM's computed data. This must have been generated
        // by a SortKeyGeneratorStage descendent in the execution tree.
        WorkingSetMember* member = _ws->get(*out);
        auto sortKeyComputedData =
            static_cast<const SortKeyComputedData*>(member->getComputed(WSM_SORT_KEY));
        BSONObj curSortKey = sortKeyComputedData->getSortKey();
        invariant(!curSortKey.isEmpty());

        if (!_prevSortKey.isEmpty() && !isInOrder(_prevSortKey, curSortKey)) {
            // 'member' is out of order. Drop it from the result set.
            _ws->free(*out);
            ++_specificStats.nDropped;
            return PlanStage::NEED_TIME;
        }

        invariant(curSortKey.isOwned());
        _prevSortKey = curSortKey;
        return PlanStage::ADVANCED;
    }

    return stageState;
}

unique_ptr<PlanStageStats> EnsureSortedStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_ENSURE_SORTED);
    ret->specific = make_unique<EnsureSortedStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* EnsureSortedStage::getSpecificStats() const {
    return &_specificStats;
}

bool EnsureSortedStage::isInOrder(const BSONObj& lhsSortKey, const BSONObj& rhsSortKey) const {
    // No need to compare with a collator, since the sort keys were extracted by the
    // SortKeyGenerator, which has already mapped strings to their comparison keys.
    return lhsSortKey.woCompare(rhsSortKey, _pattern, /*considerFieldName*/ false) <= 0;
}

}  // namespace mongo
