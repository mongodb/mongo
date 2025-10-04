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
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
}

Value ExpressionFunction::serialize(const SerializationOptions& options) const {
    MutableDocument innerOpts(Document{{"body"_sd, options.serializeLiteral(_funcSource)},
                                       {"args"_sd, _passedArgs->serialize(options)},
                                       // "lang" is purposefully not treated as a literal since it
                                       // is more of a selection of an enum
                                       {"lang"_sd, _lang}});

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

Value ExpressionFunction::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

}  // namespace mongo
