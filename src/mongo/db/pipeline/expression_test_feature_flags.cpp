/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression_test_feature_flags.h"

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/feature_flag_test_gen.h"

namespace mongo {

void ExpressionTestFeatureFlags::_validateInternal(const BSONElement& expr,
                                                   StringData testExpressionName) {
    uassert(10445700,
            str::stream() << testExpressionName
                          << " only supported input is the integer 1, but found "
                          << typeName(expr.type()),
            expr.numberInt() == 1);
}

Value ExpressionTestFeatureFlags::evaluate(const Document& root, Variables* variables) const {
    return exec::expression::evaluate(*this, root, variables);
}

// 'featureFlagBlender' is permanently enabled on the latest FCV.
REGISTER_TEST_EXPRESSION(_testFeatureFlagLatest,
                         ExpressionTestFeatureFlagLatest::parse,
                         AllowedWithApiStrict::kConditionally,
                         AllowedWithClientType::kAny,
                         &feature_flags::gFeatureFlagBlender);

boost::intrusive_ptr<Expression> ExpressionTestFeatureFlagLatest::parse(
    ExpressionContext* expCtx, BSONElement expr, const VariablesParseState& vpsIn) {
    _validateInternal(expr, kName);
    return new ExpressionTestFeatureFlagLatest(expCtx);
}

// 'featureFlagSpoon' is permanently enabled on the lastLTS FCV.
REGISTER_TEST_EXPRESSION(_testFeatureFlagLastLTS,
                         ExpressionTestFeatureFlagLatest::parse,
                         AllowedWithApiStrict::kConditionally,
                         AllowedWithClientType::kAny,
                         &feature_flags::gFeatureFlagSpoon);

boost::intrusive_ptr<Expression> ExpressionTestFeatureFlagLastLTS::parse(
    ExpressionContext* expCtx, BSONElement expr, const VariablesParseState& vpsIn) {
    _validateInternal(expr, kName);
    return new ExpressionTestFeatureFlagLastLTS(expCtx);
}
}  // namespace mongo
