/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

/**
 * Shared base class for implementing $percentile and $median window functions.
 */
class WindowFunctionPercentileCommon : public WindowFunctionState {
public:
    void add(Value value) override {
        // Only add numeric values.
        if (!value.numeric()) {
            return;
        }
        _values.insert(value.coerceToDouble());
        _memUsageBytes += sizeof(double);
    }

    void remove(Value value) override {
        // Only numeric values were added, so only numeric values need to be removed.
        if (!value.numeric()) {
            return;
        }

        auto iter = _values.find(value.coerceToDouble());
        tassert(7455904,
                "Cannot remove a value not tracked by WindowFunctionPercentile",
                iter != _values.end());
        _memUsageBytes -= sizeof(double);
        _values.erase(iter);
    }

    void reset() override {
        _values.clear();
        // updating _memUsageBytes is the responsibility of the derived classes.
    }

protected:
    explicit WindowFunctionPercentileCommon(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx), _values(std::multiset<double>()) {}

    Value computePercentile(double p) const {
        // Calculate the rank.
        const double n = _values.size();
        const double rank = std::max<double>(0, std::ceil(p * n) - 1);

        // std::multiset stores the values in ascending order, so we don't need to sort them before
        // finding the value at index 'rank'.
        auto it = _values.begin();
        std::advance(it, rank);
        return Value(*it);
    }

    // Holds all the values in the window in ascending order.
    std::multiset<double> _values;
};

class WindowFunctionPercentile : public WindowFunctionPercentileCommon {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx,
                                                       const std::vector<double>& ps) {
        return std::make_unique<WindowFunctionPercentile>(expCtx, ps);
    }

    explicit WindowFunctionPercentile(ExpressionContext* const expCtx,
                                      const std::vector<double>& ps)
        : WindowFunctionPercentileCommon(expCtx), _ps(ps) {
        _memUsageBytes = sizeof(*this) + _ps.capacity() * sizeof(double);
    }

    Value getValue() const final {
        if (_values.empty())
            return Value{BSONNULL};

        std::vector<Value> pctls;
        pctls.reserve(_ps.size());
        for (double p : _ps) {
            auto result = WindowFunctionPercentileCommon::computePercentile(p);
            pctls.push_back(result);
        }

        return Value(pctls);
    };

    void reset() final {
        WindowFunctionPercentileCommon::reset();
        _memUsageBytes = sizeof(*this) + _ps.capacity() * sizeof(double);
    }

private:
    std::vector<double> _ps;
};

class WindowFunctionMedian : public WindowFunctionPercentileCommon {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionMedian>(expCtx);
    }

    explicit WindowFunctionMedian(ExpressionContext* const expCtx)
        : WindowFunctionPercentileCommon(expCtx) {
        _memUsageBytes = sizeof(*this);
    }

    Value getValue() const final {
        if (_values.empty())
            return Value{BSONNULL};

        return WindowFunctionPercentileCommon::computePercentile(0.5 /* p */);
    }

    void reset() final {
        WindowFunctionPercentileCommon::reset();
        _memUsageBytes = sizeof(*this);
    }
};

}  // namespace mongo
