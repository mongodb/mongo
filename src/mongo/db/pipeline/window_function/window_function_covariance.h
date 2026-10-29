// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_avg.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class WindowFunctionCovariance : public WindowFunctionState {
public:
    static inline const Value kDefault = Value(BSONNULL);

    WindowFunctionCovariance(ExpressionContext* expCtx, bool isSamp);

    void add(Value value) override;

    void remove(Value value) override;

    void reset() override;

    Value getValue(boost::optional<Value> current = boost::none) const override;

    bool isSample() const {
        return _isSamp;
    }

private:
    bool _isSamp;
    long long _count = 0;

    WindowFunctionAvg _meanX;
    WindowFunctionAvg _meanY;
    WindowFunctionSum _cXY;
};

class WindowFunctionCovarianceSamp final : public WindowFunctionCovariance {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionCovarianceSamp>(expCtx);
    }

    explicit WindowFunctionCovarianceSamp(ExpressionContext* const expCtx)
        : WindowFunctionCovariance(expCtx, true) {}
};

class WindowFunctionCovariancePop final : public WindowFunctionCovariance {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionCovariancePop>(expCtx);
    }

    explicit WindowFunctionCovariancePop(ExpressionContext* const expCtx)
        : WindowFunctionCovariance(expCtx, false) {}
};

}  // namespace mongo
