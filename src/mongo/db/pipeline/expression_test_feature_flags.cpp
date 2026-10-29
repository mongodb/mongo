// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_test_feature_flags.h"

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/feature_flag_test_gen.h"

#include <string_view>

namespace mongo {

void ExpressionTestFeatureFlags::_validateInternal(const BSONElement& expr,
                                                   std::string_view testExpressionName) {
    uassert(10445700,
            str::stream() << testExpressionName
                          << " only supported input is the integer 1, but found "
                          << typeName(expr.type()),
            expr.numberInt() == 1);
}

Value ExpressionTestFeatureFlags::evaluate(const Document& root,
                                           Variables* variables,
                                           const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
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
