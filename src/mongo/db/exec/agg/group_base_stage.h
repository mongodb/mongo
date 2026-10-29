// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/group_processor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class GroupBaseStage : public Stage {
public:
    const SpecificStats* getSpecificStats() const final {
        return &_groupProcessor->getStats();
    }

    /**
     * Returns true if this $group stage used disk during execution and false otherwise.
     */
    bool usedDisk() const final {
        return _groupProcessor->usedDisk();
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const final;

protected:
    GroupBaseStage(std::string_view stageName,
                   const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                   const std::shared_ptr<GroupProcessor>& groupProcessor)
        : exec::agg::Stage(stageName, pExpCtx), _groupProcessor(groupProcessor) {};

    void doForceSpill() override {
        _groupProcessor->spill();
    }

    std::shared_ptr<GroupProcessor> _groupProcessor;

private:
    void doDispose() final {
        _groupProcessor->reset();
    }
};

}  // namespace mongo::exec::agg
