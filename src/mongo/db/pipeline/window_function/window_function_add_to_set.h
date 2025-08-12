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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/memory_token_container_util.h"
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/util/assert_util.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace mongo {

class WindowFunctionAddToSet final : public WindowFunctionState {
public:
    static inline const Value kDefault = Value{std::vector<Value>()};

    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionAddToSet>(expCtx);
    }

    explicit WindowFunctionAddToSet(ExpressionContext* const expCtx)
        : WindowFunctionState(expCtx, internalQueryMaxAddToSetBytes.load()),
          _values(MemoryTokenValueComparator(&_expCtx->getValueComparator())) {
        _memUsageTracker.set(sizeof(*this));
    }

    void add(Value value) override {
        _values.emplace(SimpleMemoryUsageToken{value.getApproximateSize(), &_memUsageTracker},
                        std::move(value));
        uassert(ErrorCodes::ExceededMemoryLimit,
                str::stream() << "$addToSet used too much memory and cannot spill to disk. Used: "
                              << _memUsageTracker.inUseTrackedMemoryBytes()
                              << " bytes. Memory limit: "
                              << _memUsageTracker.maxAllowedMemoryUsageBytes() << " bytes",
                _memUsageTracker.withinMemoryLimit());
    }

    /**
     * This should only remove the first/lowest element in the window.
     */
    void remove(Value value) override {
        auto iter = _values.find(std::move(value));
        tassert(
            5423800, "Can't remove from an empty WindowFunctionAddToSet", iter != _values.end());
        _values.erase(iter);
    }

    void reset() override {
        _values.clear();
        _memUsageTracker.set(sizeof(*this));
    }

    Value getValue(boost::optional<Value> current = boost::none) const override {
        std::vector<Value> output;
        if (_values.empty())
            return kDefault;
        for (auto it = _values.begin(); it != _values.end(); it = _values.upper_bound(*it)) {
            output.push_back(it->value());
        }

        return Value(std::move(output));
    }

private:
    std::multiset<SimpleMemoryUsageTokenWith<Value>, MemoryTokenValueComparator> _values;
};

}  // namespace mongo
