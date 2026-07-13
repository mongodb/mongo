// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"

#include <string_view>

namespace mongo::exec::agg {

/**
 * Detects {_eos: true} sentinels in the metadata consumer pipeline and disposes the Exchange
 * consumer on sight, returning EOF. This prevents deadlock when the doc consumer's buffer fills
 * before the meta consumer reaches natural EOF.
 */
class InternalStreamTerminatorStage final : public Stage {
public:
    InternalStreamTerminatorStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    GetNextResult doGetNext() final;

    // True after EOS sentinel seen. Guards against calling getNext() on a disposed source.
    bool _terminated = false;
};

}  // namespace mongo::exec::agg
