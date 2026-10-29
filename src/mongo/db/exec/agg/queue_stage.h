// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <memory.h>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {

class QueueStage final : public Stage {
public:
    QueueStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               std::deque<GetNextResult> queue);

    template <class... Args>
    GetNextResult& emplace_back(Args&&... args) {
        return _queue.emplace_back(std::forward<Args>(args)...);
    }

private:
    // Documents are always returned starting from the front.
    GetNextResult doGetNext() override;

    std::deque<GetNextResult> _queue;
};

}  // namespace mongo::exec::agg
