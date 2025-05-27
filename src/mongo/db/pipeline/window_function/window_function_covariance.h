/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_avg.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"

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
