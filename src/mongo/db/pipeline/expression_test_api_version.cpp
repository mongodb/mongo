/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/pipeline/expression_test_api_version.h"

namespace mongo {

REGISTER_TEST_EXPRESSION(_testApiVersion, ExpressionTestApiVersion::parse);

ExpressionTestApiVersion::ExpressionTestApiVersion(ExpressionContext* const expCtx,
                                                   bool unstable,
                                                   bool deprecated)
    : Expression(expCtx), _unstable(unstable), _deprecated(deprecated) {}

boost::intrusive_ptr<Expression> ExpressionTestApiVersion::parse(ExpressionContext* const expCtx,
                                                                 BSONElement expr,
                                                                 const VariablesParseState& vps) {
    uassert(5161700,
            "$_testApiVersion only supports an object as its argument",
            expr.type() == BSONType::Object);

    const BSONObj params = expr.embeddedObject();
    uassert(5161701,
            "$_testApiVersion only accepts an object with a single field.",
            params.nFields() == 1);

    bool unstableField = false;
    bool deprecatedField = false;

    auto field = params.firstElementFieldNameStringData();
    if (field == kUnstableField) {
        uassert(5161702, "unstable must be a boolean", params.firstElement().isBoolean());
        unstableField = params.firstElement().boolean();
    } else if (field == kDeprecatedField) {
        uassert(5161703, "deprecated must be a boolean", params.firstElement().isBoolean());
        deprecatedField = params.firstElement().boolean();
    } else {
        uasserted(5161704,
                  str::stream() << field << " is not a valid argument for $_testApiVersion");
    }

    return new ExpressionTestApiVersion(expCtx, unstableField, deprecatedField);
}

Value ExpressionTestApiVersion::serialize(bool explain) const {
    return Value(Document{{"$_testApiVersion",
                           Document{{"unstable", _unstable ? Value(_unstable) : Value()},
                                    {"deprecated", _deprecated ? Value(_deprecated) : Value()}}}});
}

Value ExpressionTestApiVersion::evaluate(const Document& root, Variables* variables) const {
    APIParameters apiParams = getExpressionContext()->apiParameters;

    if (apiParams.getAPIStrict().value_or(false) && _unstable) {
        uasserted(ErrorCodes::APIStrictError,
                  "Provided apiStrict is true with an unstable command.");
    }

    if (apiParams.getAPIDeprecationErrors().value_or(false) && _deprecated) {
        uasserted(ErrorCodes::APIDeprecationError,
                  "Provided apiDeprecatedErrors is true with a deprecated command.");
    }

    return Value(1);
}

void ExpressionTestApiVersion::_doAddDependencies(DepsTracker* deps) const {}

}  // namespace mongo
