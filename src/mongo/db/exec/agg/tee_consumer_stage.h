// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class TeeConsumerStage final : public Stage {
public:
    TeeConsumerStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     size_t facetId,
                     std::string_view stageName);

    void setTeeBuffer(const boost::intrusive_ptr<TeeBuffer>& bufferSource);

private:
    GetNextResult doGetNext() final;
    void doDispose() final;

    size_t _facetId;
    boost::intrusive_ptr<TeeBuffer> _bufferSource;
};

}  // namespace mongo::exec::agg
