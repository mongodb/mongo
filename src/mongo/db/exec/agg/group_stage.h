// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/group_base_stage.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

class GroupStage final : public GroupBaseStage {
public:
    GroupStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               const std::shared_ptr<GroupProcessor>& groupProcessor);

    bool isEOF() const final {
        return _groupsReady && !_groupProcessor->hasNext();
    }

private:
    GetNextResult doGetNext() final;

    /**
     * Before returning anything, this source must prepare itself. performBlockingGroup() exhausts
     * the previous source before
     * returning. The '_groupsReady' boolean indicates that performBlockingGroup() has finished.
     *
     * This method may not be able to finish initialization in a single call if 'pSource' returns a
     * DocumentSource::GetNextResult::kPauseExecution, so it returns the last GetNextResult
     * encountered, which may be either kEOF or kPauseExecution.
     */
    GetNextResult performBlockingGroup();

    /**
     * Initializes this $group after any children are initialized. See performBlockingGroup() for
     * more details.
     */
    GetNextResult performBlockingGroupSelf(GetNextResult input);

    bool _groupsReady;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
