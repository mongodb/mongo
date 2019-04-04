/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kQuery

#include "merizo/platform/basic.h"

#include "merizo/db/exec/sort_key_generator.h"

#include <vector>

#include "merizo/bson/bsonobj_comparator.h"
#include "merizo/db/catalog/collection.h"
#include "merizo/db/exec/scoped_timer.h"
#include "merizo/db/exec/working_set.h"
#include "merizo/db/exec/working_set_common.h"
#include "merizo/db/exec/working_set_computed_data.h"
#include "merizo/db/matcher/extensions_callback_noop.h"
#include "merizo/db/query/collation/collation_index_key.h"
#include "merizo/db/query/collation/collator_interface.h"
#include "merizo/stdx/memory.h"
#include "merizo/util/log.h"

namespace merizo {

const char* SortKeyGeneratorStage::kStageType = "SORT_KEY_GENERATOR";

SortKeyGeneratorStage::SortKeyGeneratorStage(OperationContext* opCtx,
                                             PlanStage* child,
                                             WorkingSet* ws,
                                             const BSONObj& sortSpecObj,
                                             const CollatorInterface* collator)
    : PlanStage(kStageType, opCtx), _ws(ws), _sortSpec(sortSpecObj), _collator(collator) {
    _children.emplace_back(child);
}

bool SortKeyGeneratorStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState SortKeyGeneratorStage::doWork(WorkingSetID* out) {
    if (!_sortKeyGen) {
        _sortKeyGen = stdx::make_unique<SortKeyGenerator>(_sortSpec, _collator);
        return PlanStage::NEED_TIME;
    }

    auto stageState = child()->work(out);
    if (stageState == PlanStage::ADVANCED) {
        WorkingSetMember* member = _ws->get(*out);

        StatusWith<BSONObj> sortKey = BSONObj();
        if (member->hasObj()) {
            SortKeyGenerator::Metadata metadata;
            if (_sortKeyGen->sortHasMeta() && member->hasComputed(WSM_COMPUTED_TEXT_SCORE)) {
                auto scoreData = static_cast<const TextScoreComputedData*>(
                    member->getComputed(WSM_COMPUTED_TEXT_SCORE));
                metadata.textScore = scoreData->getScore();
            }
            sortKey = _sortKeyGen->getSortKey(member->obj.value(), &metadata);
        } else {
            sortKey = getSortKeyFromIndexKey(*member);
        }

        if (!sortKey.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_ws, sortKey.getStatus());
            return PlanStage::FAILURE;
        }

        // Add the sort key to the WSM as computed data.
        member->addComputed(new SortKeyComputedData(sortKey.getValue()));

        return PlanStage::ADVANCED;
    }

    if (stageState == PlanStage::IS_EOF) {
        _commonStats.isEOF = true;
    }

    return stageState;
}

std::unique_ptr<PlanStageStats> SortKeyGeneratorStage::getStats() {
    auto ret = stdx::make_unique<PlanStageStats>(_commonStats, STAGE_SORT_KEY_GENERATOR);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SortKeyGeneratorStage::getSpecificStats() const {
    // No specific stats are tracked for the sort key generation stage.
    return nullptr;
}

StatusWith<BSONObj> SortKeyGeneratorStage::getSortKeyFromIndexKey(
    const WorkingSetMember& member) const {
    invariant(member.getState() == WorkingSetMember::RID_AND_IDX);
    invariant(!_sortKeyGen->sortHasMeta());

    BSONObjBuilder objBuilder;
    for (BSONElement specElt : _sortSpec) {
        invariant(specElt.isNumber());
        BSONElement sortKeyElt;
        invariant(member.getFieldDotted(specElt.fieldName(), &sortKeyElt));
        // If we were to call 'collationAwareIndexKeyAppend' with a non-simple collation and a
        // 'sortKeyElt' representing a collated index key we would incorrectly encode for the
        // collation twice. This is not currently possible as the query planner will ensure that
        // the plan fetches the data before sort key generation in the case where the index has a
        // non-simple collation.
        CollationIndexKey::collationAwareIndexKeyAppend(sortKeyElt, _collator, &objBuilder);
    }
    return objBuilder.obj();
}

}  // namespace merizo
