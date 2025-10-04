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

#include "mongo/db/exec/agg/group_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_group.h"

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

GroupStage::GroupStage(StringData stageName,
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
