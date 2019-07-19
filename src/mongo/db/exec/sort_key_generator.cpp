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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sort_key_generator.h"

#include <memory>
#include <vector>

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/log.h"

namespace mongo {

const char* SortKeyGeneratorStage::kStageType = "SORT_KEY_GENERATOR";

SortKeyGeneratorStage::SortKeyGeneratorStage(OperationContext* opCtx,
                                             PlanStage* child,
                                             WorkingSet* ws,
                                             const BSONObj& sortSpecObj,
                                             const CollatorInterface* collator)
    : PlanStage(kStageType, opCtx), _ws(ws), _sortKeyGen(sortSpecObj, collator) {
    _children.emplace_back(child);
}

bool SortKeyGeneratorStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState SortKeyGeneratorStage::doWork(WorkingSetID* out) {
    auto stageState = child()->work(out);
    if (stageState == PlanStage::ADVANCED) {
        WorkingSetMember* member = _ws->get(*out);

        auto sortKey = _sortKeyGen.getSortKey(*member);
        if (!sortKey.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_ws, sortKey.getStatus());
            return PlanStage::FAILURE;
        }

        // Add the sort key to the WSM as metadata.
        member->metadata().setSortKey(sortKey.getValue());

        return PlanStage::ADVANCED;
    }

    if (stageState == PlanStage::IS_EOF) {
        _commonStats.isEOF = true;
    }

    return stageState;
}

std::unique_ptr<PlanStageStats> SortKeyGeneratorStage::getStats() {
    auto ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_SORT_KEY_GENERATOR);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SortKeyGeneratorStage::getSpecificStats() const {
    // No specific stats are tracked for the sort key generation stage.
    return nullptr;
}

}  // namespace mongo
