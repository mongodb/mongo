// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class SkipStage final : public Stage {
public:
    SkipStage(std::string_view stageName,
              const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
              long long nToSkip);

    long long getSkip() const {
        return _nToSkip;
    }
    void setSkip(long long newSkip) {
        _nToSkip = newSkip;
    }

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

private:
    GetNextResult doGetNext() final;

    long long _nToSkip = 0;
    long long _nSkippedSoFar = 0;
};

}  // namespace mongo::exec::agg
