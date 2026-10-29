// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalShardServerInfoStage final : public Stage {
public:
    InternalShardServerInfoStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
        : Stage(stageName, pExpCtx) {}

private:
    GetNextResult doGetNext() final;

    bool _didEmit = false;
};

}  // namespace mongo::exec::agg
