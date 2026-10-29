// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class LimitStage final : public Stage {
public:
    LimitStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               long long limit);

    long long getLimit() const {
        return _limit;
    }
    void setLimit(long long newLimit) {
        _limit = newLimit;
    }

    bool isEOF() const final {
        return _nReturned >= _limit || (pSource && pSource->isEOF());
    }

private:
    GetNextResult doGetNext() final;

    long long _limit;
    long long _nReturned = 0;
};

}  // namespace mongo::exec::agg
