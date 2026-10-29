// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/group_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_group.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceGroupToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto groupDS = boost::dynamic_pointer_cast<DocumentSourceGroup>(documentSource);

    tassert(10422900, "expected 'DocumentSourceGroup' type", groupDS);

    return make_intrusive<exec::agg::GroupStage>(
        groupDS->kStageName, groupDS->getExpCtx(), groupDS->_groupProcessor);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(group, DocumentSourceGroup::id, documentSourceGroupToStageFn)

GroupStage::GroupStage(std::string_view stageName,
                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       const std::shared_ptr<GroupProcessor>& groupProcessor)
    : GroupBaseStage(stageName, pExpCtx, groupProcessor), _groupsReady(false) {};

GetNextResult GroupStage::doGetNext() {
    if (!_groupsReady) {
        auto initializationResult = performBlockingGroup();
        if (initializationResult.isPaused()) {
            return initializationResult;
        }
        invariant(initializationResult.isEOF());
    }

    auto result = _groupProcessor->getNext();
    if (!result) {
        dispose();
        return GetNextResult::makeEOF();
    }
    return GetNextResult(std::move(*result));
}

GetNextResult GroupStage::performBlockingGroup() {
    GetNextResult input = pSource->getNext();
    return performBlockingGroupSelf(input);
}

// This separate NOINLINE function is used here to decrease stack utilization of
// performBlockingGroup() and prevent stack overflows.
MONGO_COMPILER_NOINLINE GetNextResult GroupStage::performBlockingGroupSelf(GetNextResult input) {
    _groupProcessor->setExecutionStarted();
    // Barring any pausing, this loop exhausts 'pSource' and populates '_groups'.
    for (; input.isAdvanced(); input = pSource->getNext()) {
        // We release the result document here so that it does not outlive the end of this loop
        // iteration. Not releasing could lead to an array copy when this group follows an unwind.
        auto rootDocument = input.releaseDocument();
        Value groupKey = _groupProcessor->computeGroupKey(rootDocument);
        _groupProcessor->add(groupKey, rootDocument);
    }

    switch (input.getStatus()) {
        case GetNextResult::ReturnStatus::kAdvanced: {
            MONGO_UNREACHABLE;  // We consumed all advances above.
        }
        case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
            tasserted(10358900, "Group does not support control events");
        }
        case GetNextResult::ReturnStatus::kPauseExecution: {
            return input;  // Propagate pause.
        }
        case GetNextResult::ReturnStatus::kEOF: {
            _groupProcessor->readyGroups();
            // This must happen last so that, unless control gets here, we will re-enter
            // initialization after getting a GetNextResult::ResultState::kPauseExecution.
            _groupsReady = true;
            return input;
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
