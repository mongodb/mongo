// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/percentile_algo_continuous.h"
#include "mongo/db/pipeline/percentile_algo_discrete.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <typename TAccumulator>
class ExpressionFromAccumulatorQuantile : public Expression {
public:
    explicit ExpressionFromAccumulatorQuantile(ExpressionContext* const expCtx,
                                               const std::vector<double>& ps,
                                               boost::intrusive_ptr<Expression> input,
                                               PercentileMethodEnum method)
        : Expression(expCtx, {input}), _ps(ps), _input(input), _method(method) {
        expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);
    }

    const char* getOpName() const {
        return TAccumulator::kName.data();
    }

    Value serialize(const query_shape::SerializationOptions& options = {}) const final {
        MutableDocument md;
        TAccumulator::serializeHelper(_input, options, _ps, _method, md);
        return Value(DOC(getOpName() << md.freeze()));
    }

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const std::vector<double>& getPs() const {
        return _ps;
    }

    const Expression* getInput() const {
        return _input.get();
    }

    PercentileMethodEnum getMethod() const {
        return _method;
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionFromAccumulatorQuantile<TAccumulator>>(
            &expCtx, _ps, cloneChild(0, expCtx), _method);
    }

private:
    std::vector<double> _ps;
    boost::intrusive_ptr<Expression> _input;
    PercentileMethodEnum _method;

    template <typename H>
    friend class ExpressionHashVisitor;
};

}  // namespace mongo
