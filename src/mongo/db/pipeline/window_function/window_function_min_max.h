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

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMax : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{BSONNULL};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionMinMax<sense>>(expCtx);
    }

    explicit WindowFunctionMinMax(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx),
          _values(_expCtx->getValueComparator().makeOrderedValueMultiset()) {
        _memUsageBytes = sizeof(*this);
    }

    void add(Value value) final {
        // Ignore nullish values.
        if (value.nullish())
            return;
        _memUsageBytes += value.getApproximateSize();
        _values.insert(std::move(value));
    }

    void remove(Value value) final {
        // Ignore nullish values.
        if (value.nullish())
            return;
        // std::multiset::insert is guaranteed to put the element after any equal elements
        // already in the container. So find() / erase() will remove the oldest equal element,
        // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
        auto iter = _values.find(std::move(value));
        tassert(5371400, "Can't remove from an empty WindowFunctionMinMax", iter != _values.end());
        _memUsageBytes -= iter->getApproximateSize();
        _values.erase(iter);
    }

    void reset() final {
        _values.clear();
        _memUsageBytes = sizeof(*this);
    }

    Value getValue() const final {
        if (_values.empty())
            return kDefault;
        switch (sense) {
            case AccumulatorMinMax::Sense::kMin:
                return *_values.begin();
            case AccumulatorMinMax::Sense::kMax:
                return *_values.rbegin();
        }
        MONGO_UNREACHABLE_TASSERT(5371401);
    }

protected:
    // Holds all the values in the window, in order, with constant-time access to both ends.
    ValueMultiset _values;
};
using WindowFunctionMin = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMin>;
using WindowFunctionMax = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMax>;

}  // namespace mongo
