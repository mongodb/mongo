// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/change_stream_event_transform.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream transform aggregation stage and is
 * part of the execution pipeline. Its construction is based on DocumentSourceChangeStreamTransform,
 * which handles the optimization part.
 */
class ChangeStreamTransformStage final : public Stage {
public:
    ChangeStreamTransformStage(std::string_view stageName,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               std::shared_ptr<ChangeStreamEventTransformer> transformer);

private:
    GetNextResult doGetNext() final;

    std::shared_ptr<ChangeStreamEventTransformer> _transformer;
};
}  // namespace mongo::exec::agg
