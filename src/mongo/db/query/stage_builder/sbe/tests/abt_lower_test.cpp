// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/abt_lower.h"

#include "mongo/db/query/stage_builder/sbe/abt/explain.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_literals.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/unittest.h"

namespace mongo::stage_builder::abt_lower {
namespace {
using namespace std::literals::string_view_literals;

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query/stage_builder/sbe"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
using namespace unit_test_abt_literals;
using namespace abt;
class ABTPlanGeneration : public unittest::Test {
protected:
    sbe::InputParamToSlotMap inputParamToSlotMap;

    void runExpressionVariation(GoldenTestContext& gctx, const std::string& name, const ABT& n) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        auto expr =
            SBEExpressionLowering{env, map, runtimeEnv, ids, inputParamToSlotMap}.optimize(n);
        stream << expr->toString() << std::endl;
    }

    std::string autoUpdateExpressionVariation(const ABT& n) {
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        auto expr =
            SBEExpressionLowering{env, map, runtimeEnv, ids, inputParamToSlotMap}.optimize(n);
        return expr->toString();
    }
};

TEST_F(ABTPlanGeneration, LowerConstantExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runExpressionVariation(ctx, "string", Constant::str("hello world"sv));

    runExpressionVariation(ctx, "int64", Constant::int64(100));
    runExpressionVariation(ctx, "int32", Constant::int32(32));
    runExpressionVariation(ctx, "double", Constant::fromDouble(3.14));
    runExpressionVariation(ctx, "decimal", Constant::fromDecimal(Decimal128("3.14")));

    runExpressionVariation(ctx, "timestamp", Constant::timestamp(Timestamp::max()));
    runExpressionVariation(ctx, "date", Constant::date(Date_t::fromMillisSinceEpoch(100)));

    runExpressionVariation(ctx, "boolean true", Constant::boolean(true));
    runExpressionVariation(ctx, "boolean false", Constant::boolean(false));
}

TEST_F(ABTPlanGeneration, LowerBinaryOpEqMemberRHSArray) {
    // Lower BinaryOp [EqMember] where the type of RHS is array.
    std::string output = autoUpdateExpressionVariation(
        _binary("EqMember", "hello"_cstr, _carray("1"_cdouble, "2"_cdouble, "3"_cdouble))._n);

    ASSERT_STR_EQ_AUTO(  // NOLINT
        "isMember(\"hello\", [1L, 2L, 3L]) ",
        output);
}

}  // namespace
}  // namespace mongo::stage_builder::abt_lower
