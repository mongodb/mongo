// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/agg/unwind_processor.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

/**
 * This class handles the execution part of the unwind aggregation stage and is part of the
 * execution pipeline. Its construction is based on DocumentSourceUnwind, which handles the
 * optimization part.
 */
class UnwindStage final : public Stage {
public:
    UnwindStage(std::string_view stageName,
                const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                std::unique_ptr<UnwindProcessor> unwindProcessor);

    bool isEOF() const final {
        return !_unwindProcessor->haveNext() && pSource && pSource->isEOF();
    }

private:
    GetNextResult doGetNext() final;

    std::unique_ptr<UnwindProcessor> _unwindProcessor;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
