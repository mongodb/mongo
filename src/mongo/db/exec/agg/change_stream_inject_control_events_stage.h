// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream inject control events aggregation
 * stage and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamInjectControlEvents, which handles the optimization part.
 */
class ChangeStreamInjectControlEventsStage final : public Stage {
public:
    ChangeStreamInjectControlEventsStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        DocumentSourceChangeStreamInjectControlEvents::ActionsMap _actions);

private:
    GetNextResult doGetNext() final;

    /**
     * An optional buffered control event. If set, it will be emitted on the next call to
     * 'doGetNext()' and then resetted to none.
     */
    boost::optional<GetNextResult> _bufferedControlEvent = boost::none;

    /**
     * A mapping from event name to the actions to take.
     */
    DocumentSourceChangeStreamInjectControlEvents::ActionsMap _actions;
};
}  // namespace mongo::exec::agg
