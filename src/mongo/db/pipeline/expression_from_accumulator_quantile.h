/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <vector>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_discrete.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/safe_num.h"

namespace mongo {

template <typename TAccumulator>
class ExpressionFromAccumulatorQuantile : public Expression {
public:
    explicit ExpressionFromAccumulatorQuantile(ExpressionContext* const expCtx,
                                               std::vector<double>& ps,
                                               boost::intrusive_ptr<Expression> input,
                                               PercentileMethod method)
        : Expression(expCtx, {input}), _ps(ps), _input(input), _method(method) {
        expCtx->sbeCompatibility = SbeCompatibility::notCompatible;
    }

    const char* getOpName() const {
        return TAccumulator::kName.rawData();
    }

    Value serialize(const SerializationOptions& options = {}) const final {
        MutableDocument md;
        TAccumulator::serializeHelper(_input, options, _ps, _method, md);
        return Value(DOC(getOpName() << md.freeze()));
    }

    Value evaluate(const Document& root, Variables* variables) const final {
        auto input = _input->evaluate(root, variables);
        if (input.numeric()) {
            // On a scalar value, all percentiles are the same for all methods.
            return TAccumulator::formatFinalValue(
                _ps.size(), std::vector<double>(_ps.size(), input.coerceToDouble()));
        }

        if (input.isArray() && input.getArrayLength() > 0) {
            if (_method != PercentileMethod::Continuous) {
                // On small datasets, which are likely to be the inputs for the expression, creating
                // t-digests is inefficient, so instead we use DiscretePercentile algo directly for
                // both "discrete" and "approximate" methods.
                std::vector<double> samples;
                samples.reserve(input.getArrayLength());
                for (const auto& item : input.getArray()) {
                    if (item.numeric()) {
                        samples.push_back(item.coerceToDouble());
                    }
                }
                DiscretePercentile dp;
                dp.incorporate(samples);
                return TAccumulator::formatFinalValue(_ps.size(), dp.computePercentiles(_ps));
            } else {
                // Delegate to the accumulator. Note: it would be more efficient to use the
                // percentile algorithms directly rather than an accumulator, as it would reduce
                // heap alloc, virtual calls and avoid unnecessary for expressions memory tracking.
                // This path currently cannot be executed as we only support discrete percentiles.
                TAccumulator accum(this->getExpressionContext(), _ps, _method);
                for (const auto& item : input.getArray()) {
                    accum.process(item, false /* merging */);
                }
                return accum.getValue(false /* toBeMerged */);
            }
        }

        // No numeric values have been found for the expression to process.
        return TAccumulator::formatFinalValue(_ps.size(), {});
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }


private:
    std::vector<double> _ps;
    boost::intrusive_ptr<Expression> _input;
    PercentileMethod _method;

    template <typename H>
    friend class ExpressionHashVisitor;
};

}  // namespace mongo
