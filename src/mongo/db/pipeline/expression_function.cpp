// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_function.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_STABLE_EXPRESSION(function, ExpressionFunction::parse);

ExpressionFunction::ExpressionFunction(ExpressionContext* const expCtx,
                                       boost::intrusive_ptr<Expression> passedArgs,
                                       bool assignFirstArgToThis,
                                       std::string funcSource,
                                       std::string lang)
    : Expression(expCtx, {std::move(passedArgs)}),
      _passedArgs(_children[0]),
      _assignFirstArgToThis(assignFirstArgToThis),
      _funcSource(std::move(funcSource)),
      _lang(std::move(lang)) {
    expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);
}

Value ExpressionFunction::serialize(const query_shape::SerializationOptions& options) const {
    MutableDocument innerOpts(Document{{"body"sv, options.serializeLiteral(_funcSource)},
                                       {"args"sv, _passedArgs->serialize(options)},
                                       // "lang" is purposefully not treated as a literal since it
                                       // is more of a selection of an enum
                                       {"lang"sv, _lang}});

    // This field will only be serialized when desugaring $where in $expr + $_internalJs
    if (_assignFirstArgToThis) {
        innerOpts["_internalSetObjToThis"] = options.serializeLiteral(_assignFirstArgToThis);
    }
    return Value(Document{{kExpressionName, innerOpts.freezeToValue()}});
}

boost::intrusive_ptr<Expression> ExpressionFunction::parse(ExpressionContext* const expCtx,
                                                           BSONElement expr,
                                                           const VariablesParseState& vps) {
    expCtx->setServerSideJsConfigFunction(true);

    uassert(4660800,
            str::stream() << kExpressionName << " cannot be used inside a validator.",
            !expCtx->getIsParsingCollectionValidator());

    uassert(31260,
            str::stream() << kExpressionName
                          << " requires an object as an argument, found: " << expr.type(),
            expr.type() == BSONType::object);

    BSONElement bodyField = expr["body"];

    uassert(31261, "The body function must be specified.", bodyField);

    boost::intrusive_ptr<Expression> bodyExpr = Expression::parseOperand(expCtx, bodyField, vps);

    auto bodyConst = dynamic_cast<ExpressionConstant*>(bodyExpr.get());
    uassert(31432, "The body function must be a constant expression", bodyConst);

    auto bodyValue = bodyConst->getValue();
    uassert(31262,
            "The body function must evaluate to type string or code",
            bodyValue.getType() == BSONType::string || bodyValue.getType() == BSONType::code);

    BSONElement argsField = expr["args"];
    uassert(31263, "The args field must be specified.", argsField);
    boost::intrusive_ptr<Expression> argsExpr = parseOperand(expCtx, argsField, vps);

    // This element will be present when desugaring $where, only.
    BSONElement assignFirstArgToThis = expr["_internalSetObjToThis"];

    BSONElement langField = expr["lang"];
    uassert(31418, "The lang field must be specified.", langField);
    uassert(31419,
            "Currently the only supported language specifier is 'js'.",
            langField.type() == BSONType::string && langField.str() == kJavaScript);

    return new ExpressionFunction(expCtx,
                                  argsExpr,
                                  assignFirstArgToThis.trueValue(),
                                  bodyValue.coerceToString(),
                                  langField.str());
}

Value ExpressionFunction::evaluate(const Document& root,
                                   Variables* variables,
                                   const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

}  // namespace mongo
