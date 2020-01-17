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

    StringData _funcSource;
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
        std::string finalize);

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
                  std::string finalize)
        : AccumulatorState(expCtx),
          _init(init),
          _accumulate(accumulate),
          _merge(merge),
          _finalize(finalize) {
        recomputeMemUsageBytes();
    }
    void recomputeMemUsageBytes();

    // static arguments
    std::string _init, _accumulate, _merge, _finalize;

    // accumulator state during execution
    // - When the accumulator is first created, _state is empty.
    // - When the accumulator is fed its first input Value, it runs the user init and accumulate
    //   functions, and _state gets a Value.
    // - When the accumulator is reset, _state becomes empty again.
    std::optional<Value> _state;
};

}  // namespace mongo
