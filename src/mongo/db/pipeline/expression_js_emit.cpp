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
    expCtx->setSbeCompatibility(SbeCompatibility::notCompatible);
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

Value ExpressionInternalJsEmit::serialize(const SerializationOptions& options) const {
    return Value(
        Document{{kExpressionName,
                  Document{{"eval", _funcSource}, {"this", _thisRef->serialize(options)}}}});
}

Value ExpressionInternalJsEmit::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}
}  // namespace mongo
