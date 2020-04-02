/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

class AccumulatorInternalJsReduce final : public AccumulatorState {
public:
    static constexpr auto kAccumulatorName = "$_internalJsReduce"_sd;

    static boost::intrusive_ptr<AccumulatorState> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, StringData funcSource);

    static AccumulationExpression parseInternalJsReduce(
        boost::intrusive_ptr<ExpressionContext> expCtx, BSONElement elem, VariablesParseState vps);

    AccumulatorInternalJsReduce(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                StringData funcSource)
        : AccumulatorState(expCtx), _funcSource(funcSource) {
        _memUsageBytes = sizeof(*this);
    }

    const char* getOpName() const final {
        return kAccumulatorName.rawData();
    }

    void processInternal(const Value& input, bool merging) final;

    Value getValue(bool toBeMerged) final;

    void reset() final;

    virtual Document serialize(boost::intrusive_ptr<Expression> initializer,
                               boost::intrusive_ptr<Expression> argument,
                               bool explain) const override;

private:
    static std::string parseReduceFunction(BSONElement func);

    std::string _funcSource;
    std::vector<Value> _values;
    Value _key;
};

class AccumulatorJs final : public AccumulatorState {
public:
    static constexpr auto kAccumulatorName = "$accumulator"_sd;
    const char* getOpName() const final {
        return kAccumulatorName.rawData();
    }

    // An AccumulatorState instance only owns its "static" arguments: those that don't need to be
    // evaluated per input document.
    static boost::intrusive_ptr<AccumulatorState> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::string init,
        std::string accumulate,
        std::string merge,
        boost::optional<std::string> finalize);

    static AccumulationExpression parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                                        BSONElement elem,
                                        VariablesParseState vps);

    Value getValue(bool toBeMerged) final;
    void reset() final;
    void processInternal(const Value& input, bool merging) final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       bool explain) const final;
    void startNewGroup(Value const& input) final;

private:
    AccumulatorJs(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  std::string init,
                  std::string accumulate,
                  std::string merge,
                  boost::optional<std::string> finalize)
        : AccumulatorState(expCtx),
          _init(std::move(init)),
          _accumulate(std::move(accumulate)),
          _merge(std::move(merge)),
          _finalize(std::move(finalize)) {
        resetMemUsageBytes();
    }
    void resetMemUsageBytes();
    void incrementMemUsageBytes(size_t bytes);

    // static arguments
    std::string _init, _accumulate, _merge;
    boost::optional<std::string> _finalize;

    // accumulator state during execution
    // 1. Initially, _state is empty.
    // 2. On .startNewGroup(...), _state becomes the result of the user's init function.
    // 3. On .processInternal(...), instead of calling the user's accumulate or merge function right
    //    away, we push_back the argument into _pendingCalls to be processed later. This is an
    //    optimization to reduce the number of calls into the JS engine.
    // 4. On .getValue(), we process all the _pendingCalls and update the _state.
    // 5. On .reset(), _state becomes empty again.
    boost::optional<Value> _state;
    // Each element is an input passed to processInternal.
    std::vector<Value> _pendingCalls;
    // True means the elements of _pendingCalls should be interpreted as intermediate states from
    // other instances of $accumulator. False means the elements of _pendingCalls should be
    // interpreted as inputs from accumulateArgs.
    bool _pendingCallsMerging;

    // Call the user's accumulate/merge function for each element of _pendingCalls.
    void reducePendingCalls();
};

}  // namespace mongo
