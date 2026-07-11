// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * This expression will be used to validate that versioning code is working as expected.
 * $_testApiVersion should only take one parameter, either {unstable: true} or {deprecated: true}.
 * If no error is thrown, this expression will return an integer value.
 */
class ExpressionTestApiVersion final : public Expression {
public:
    static constexpr auto kUnstableField = "unstable";
    static constexpr auto kDeprecatedField = "deprecated";

    ExpressionTestApiVersion(ExpressionContext* expCtx, bool unstable, bool deprecated);

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    Value serialize(const query_shape::SerializationOptions& options) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionTestApiVersion>(&expCtx, _unstable, _deprecated);
    }

private:
    bool _unstable;
    bool _deprecated;
};
}  // namespace mongo
