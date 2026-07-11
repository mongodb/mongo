// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class AccumulatorInternalJsReduce final : public AccumulatorState {
public:
    static constexpr auto kName = "$_internalJsReduce"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    static AccumulationExpression parseInternalJsReduce(ExpressionContext* expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps);

    AccumulatorInternalJsReduce(ExpressionContext* const expCtx, std::string_view funcSource)
        : AccumulatorState(expCtx), _funcSource(funcSource) {
        _memUsageTracker.set(sizeof(*this));
    }

    void processInternal(const Value& input, bool merging) final;

    Value getValue(bool toBeMerged) final;

    void reset() final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       const query_shape::SerializationOptions& options = {}) const override;

private:
    static std::string parseReduceFunction(BSONElement func);

    std::string _funcSource;
    std::vector<Value> _values;
    Value _key;
};

class AccumulatorJs final : public AccumulatorState {
public:
    static constexpr auto kName = "$accumulator"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    // An AccumulatorState instance only owns its "static" arguments: those that don't need to be
    // evaluated per input document.
    AccumulatorJs(ExpressionContext* const expCtx,
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

    static AccumulationExpression parse(ExpressionContext* expCtx,
                                        BSONElement elem,
                                        VariablesParseState vps);

    Value getValue(bool toBeMerged) final;
    void reset() final;
    void processInternal(const Value& input, bool merging) final;
    void reduceMemoryConsumptionIfAble() final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       const query_shape::SerializationOptions& options = {}) const final;
    void startNewGroup(Value const& input) final;

private:
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
    bool _pendingCallsMerging = false;
};

}  // namespace mongo
