// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::exec::agg {

class InternalInhibitOptimizationStage final : public Stage {
public:
    InternalInhibitOptimizationStage(std::string_view stageName,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx);

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

private:
    GetNextResult doGetNext() final;
};

}  // namespace mongo::exec::agg
