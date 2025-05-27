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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_continuous.h"
#include "mongo/db/pipeline/percentile_algo_discrete.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/safe_num.h"

#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <typename TAccumulator>
class ExpressionFromAccumulatorQuantile : public Expression {
public:
    explicit ExpressionFromAccumulatorQuantile(ExpressionContext* const expCtx,
                                               std::vector<double>& ps,
                                               boost::intrusive_ptr<Expression> input,
                                               PercentileMethodEnum method)
        : Expression(expCtx, {input}), _ps(ps), _input(input), _method(method) {
        expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
    }

    const char* getOpName() const {
        return TAccumulator::kName.data();
    }

    Value serialize(const SerializationOptions& options = {}) const final {
        MutableDocument md;
        TAccumulator::serializeHelper(_input, options, _ps, _method, md);
        return Value(DOC(getOpName() << md.freeze()));
    }

    Value evaluate(const Document& root, Variables* variables) const final;

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

private:
    std::vector<double> _ps;
    boost::intrusive_ptr<Expression> _input;
    PercentileMethodEnum _method;

    template <typename H>
    friend class ExpressionHashVisitor;
};

}  // namespace mongo
