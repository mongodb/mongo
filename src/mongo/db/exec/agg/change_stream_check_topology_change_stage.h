// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream check topology change aggregation
 * stage and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamCheckTopologyChange, which handles the optimization part.
 *
 * TODO SERVER-112325: Remove this stage once all mongos versions are guaranteed to not generate
 * this stage in a change stream pipeline anymore.
 */
class ChangeStreamCheckTopologyChangeStage final : public Stage {
public:
    ChangeStreamCheckTopologyChangeStage(std::string_view stageName,
                                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;
};
}  // namespace mongo::exec::agg
