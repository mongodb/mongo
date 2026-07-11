// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_js_emit.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

#include <cstddef>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_STABLE_EXPRESSION(_internalJsEmit, ExpressionInternalJsEmit::parse);

ExpressionInternalJsEmit::ExpressionInternalJsEmit(ExpressionContext* const expCtx,
                                                   boost::intrusive_ptr<Expression> thisRef,
                                                   std::string funcSource)
    : Expression(expCtx, {std::move(thisRef)}),
      _thisRef(_children[0]),
      _funcSource(std::move(funcSource)) {
    expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);
}

boost::intrusive_ptr<Expression> ExpressionInternalJsEmit::parse(ExpressionContext* const expCtx,
                                                                 BSONElement expr,
                                                                 const VariablesParseState& vps) {

    uassert(4660801,
            str::stream() << kExpressionName << " cannot be used inside a validator.",
            !expCtx->getIsParsingCollectionValidator());

    uassert(31221,
            str::stream() << kExpressionName
                          << " requires an object as an argument, found: " << typeName(expr.type()),
            expr.type() == BSONType::object);

    BSONElement evalField = expr["eval"];

    uassert(31222, str::stream() << "The map function must be specified.", evalField);
    uassert(31224,
            "The map function must be of type string or code",
            evalField.type() == BSONType::string || evalField.type() == BSONType::code);

    std::string funcSourceString = evalField._asCode();
    BSONElement thisField = expr["this"];
    uassert(
        31223, str::stream() << kExpressionName << " requires 'this' to be specified", thisField);
    boost::intrusive_ptr<Expression> thisRef = parseOperand(expCtx, thisField, vps);
    return new ExpressionInternalJsEmit(expCtx, std::move(thisRef), std::move(funcSourceString));
}

Value ExpressionInternalJsEmit::serialize(const query_shape::SerializationOptions& options) const {
    return Value(
        Document{{kExpressionName,
                  Document{{"eval", _funcSource}, {"this", _thisRef->serialize(options)}}}});
}

Value ExpressionInternalJsEmit::evaluate(const Document& root,
                                         Variables* variables,
                                         const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}
}  // namespace mongo
