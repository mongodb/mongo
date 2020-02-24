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

class AccumulatorInternalJsReduce final : public Accumulator {
public:
    static constexpr auto kAccumulatorName = "$_internalJsReduce"_sd;

    static boost::intrusive_ptr<Accumulator> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, StringData funcSource);

    static std::pair<boost::intrusive_ptr<Expression>, Accumulator::Factory> parseInternalJsReduce(
        boost::intrusive_ptr<ExpressionContext> expCtx, BSONElement elem, VariablesParseState vps);

    AccumulatorInternalJsReduce(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                StringData funcSource)
        : Accumulator(expCtx), _funcSource(funcSource) {
        _memUsageBytes = sizeof(*this);
    }

    const char* getOpName() const final {
        return kAccumulatorName.rawData();
    }

    void processInternal(const Value& input, bool merging) final;

    Value getValue(bool toBeMerged) final;

    void reset() final;

    virtual Document serialize(boost::intrusive_ptr<Expression> expression,
                               bool explain) const override;

private:
    static std::string parseReduceFunction(BSONElement func);

    StringData _funcSource;
    std::vector<Value> _values;
    Value _key;
};

}  // namespace mongo
