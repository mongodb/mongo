// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/util/modules.h"

namespace mongo::exec::agg {

/**
 * Pure-passthrough execution stage for $_internalHybridSearch (every DocumentSource reaching the
 * exec layer needs a registered stage mapping).
 */
class InternalHybridSearchStage final : public Stage {
public:
    InternalHybridSearchStage(std::string_view stageName,
                              const boost::intrusive_ptr<ExpressionContext>& expCtx);

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

private:
    GetNextResult doGetNext() final;
};

}  // namespace mongo::exec::agg
