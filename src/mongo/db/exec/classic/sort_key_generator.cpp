// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/sort_key_generator.h"

#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {


SortKeyGeneratorStage::SortKeyGeneratorStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             std::unique_ptr<PlanStage> child,
                                             WorkingSet* ws,
                                             const BSONObj& sortSpecObj)
    : PlanStage(kStageType, expCtx.get()),
      _ws(ws),
      _sortKeyGen({{sortSpecObj, expCtx}, expCtx->getCollator()}) {
    _children.emplace_back(std::move(child));
}

bool SortKeyGeneratorStage::isEOF() const {
    return child()->isEOF();
}

PlanStage::StageState SortKeyGeneratorStage::doWork(WorkingSetID* out) {
    auto stageState = child()->work(out);
    if (stageState == PlanStage::ADVANCED) {
        WorkingSetMember* member = _ws->get(*out);

        auto sortKey = _sortKeyGen.computeSortKey(*member);

        // Add the sort key to the WSM as metadata.
        member->metadata().setSortKey(std::move(sortKey), _sortKeyGen.isSingleElementKey());
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
