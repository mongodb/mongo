/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/abt/sbe_abt_test_util.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/ce/sampling_estimator.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"


namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;

TEST_F(ABTSBE, Lower1) {
    auto tree = Constant::int64(100);
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
    ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 100);
}

TEST_F(ABTSBE, Lower2) {
    auto tree =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(100)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
    ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 200);
}

TEST_F(ABTSBE, Lower3) {
    auto tree = make<FunctionCall>("isNumber", makeSeq(Constant::int64(10)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto result = runCompiledExpressionPredicate(compiledExpr.get());

    ASSERT(result);
}

TEST_F(ABTSBE, Lower4) {
    auto [tagArr, valArr] = sbe::value::makeNewArray();
    auto arr = sbe::value::getArrayView(valArr);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 1);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 2);
    auto [tagArrNest, valArrNest] = sbe::value::makeNewArray();
    auto arrNest = sbe::value::getArrayView(valArrNest);
    arrNest->push_back(sbe::value::TypeTags::NumberInt64, 21);
    arrNest->push_back(sbe::value::TypeTags::NumberInt64, 22);
    arr->push_back(tagArrNest, valArrNest);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 3);

    auto tree = make<FunctionCall>(
        "traverseP",
        makeSeq(make<Constant>(tagArr, valArr),
                make<LambdaAbstraction>(
                    "x", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(10))),
                Constant::nothing()));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);

    ASSERT_EQ(sbe::value::TypeTags::Array, resultTag);
}

TEST_F(ABTSBE, Lower5) {
    auto tree = make<FunctionCall>(
        "setField", makeSeq(Constant::nothing(), Constant::str("fieldA"), Constant::int64(10)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);
}

TEST_F(ABTSBE, Lower6) {
    auto prefixId = PrefixId::createForTests();

    auto [tagObj, valObj] = sbe::value::makeNewObject();
    auto obj = sbe::value::getObjectView(valObj);

    auto [tagObjIn, valObjIn] = sbe::value::makeNewObject();
    auto objIn = sbe::value::getObjectView(valObjIn);
    objIn->push_back("fieldB", sbe::value::TypeTags::NumberInt64, 100);
    obj->push_back("fieldA", tagObjIn, valObjIn);

    sbe::value::OwnedValueAccessor accessor;
    auto slotId = bindAccessor(&accessor);
    SlotVarMap map;
    map["root"] = slotId;

    accessor.reset(tagObj, valObj);

    auto tree = make<EvalPath>(
        make<PathField>(
            "fieldA",
            make<PathTraverse>(
                PathTraverse::kUnlimited,
                make<PathComposeM>(
                    make<PathField>("fieldB", make<PathDefault>(Constant::int64(0))),
                    make<PathField>("fieldC", make<PathConstant>(Constant::int64(50)))))),
        make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathLowering{prefixId}.optimize(tree)) {
            changed = true;
            env.rebuild(tree);
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);

    ASSERT(sbe::value::isObject(resultTag));
}

TEST_F(ABTSBE, Lower7) {
    auto prefixId = PrefixId::createForTests();

    auto [tagArr, valArr] = sbe::value::makeNewArray();
    auto arr = sbe::value::getArrayView(valArr);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 1);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 2);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 3);

    auto [tagObj, valObj] = sbe::value::makeNewObject();
    auto obj = sbe::value::getObjectView(valObj);
    obj->push_back("fieldA", tagArr, valArr);

    sbe::value::OwnedValueAccessor accessor;
    auto slotId = bindAccessor(&accessor);
    SlotVarMap map;
    map["root"] = slotId;

    accessor.reset(tagObj, valObj);
    auto tree = make<EvalFilter>(
        make<PathGet>("fieldA",
                      make<PathTraverse>(PathTraverse::kSingleLevel,
                                         make<PathCompare>(Operations::Eq, Constant::int64(2)))),
        make<Variable>("root"));

    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathLowering{prefixId}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);
    auto compiledExpr = compileExpression(*expr);
    auto result = runCompiledExpressionPredicate(compiledExpr.get());

    ASSERT(result);
}

TEST_F(ABTSBE, LowerFunctionCallFail) {
    std::string errorMessage = "Error: Bad value 123456789!";

    auto tree =
        make<FunctionCall>("fail",
                           makeSeq(Constant::int32(static_cast<int32_t>(ErrorCodes::BadValue)),
                                   Constant::str(errorMessage)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    Status status = Status::OK();

    try {
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::releaseValue(resultTag, resultVal);
    } catch (const DBException& e) {
        status = e.toStatus();
    }

    ASSERT(!status.isOK());
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_EQ(status.reason(), errorMessage);
}

TEST_F(ABTSBE, LowerFunctionCallConvert) {
    sbe::value::OwnedValueAccessor inputAccessor;
    auto slotId = bindAccessor(&inputAccessor);
    SlotVarMap map;
    map["inputVar"] = slotId;

    auto tree = make<FunctionCall>(
        "convert",
        makeSeq(make<Variable>("inputVar"),
                Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
    auto env = VariableEnvironment::build(tree);
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);

    {
        inputAccessor.reset(sbe::value::TypeTags::NumberDouble,
                            sbe::value::bitcastFrom<double>(42.0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(resultTag, resultVal);
        ASSERT_EQ(resultTag, sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 42);
    }

    {
        auto [tag, val] = sbe::value::makeCopyDecimal(Decimal128{-73});
        inputAccessor.reset(tag, val);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(resultTag, resultVal);
        ASSERT_EQ(resultTag, sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), -73);
    }
}

TEST_F(ABTSBE, LowerFunctionCallTypeMatch) {
    sbe::value::OwnedValueAccessor inputAccessor;
    auto slotId = bindAccessor(&inputAccessor);
    SlotVarMap map;
    map["inputVar"] = slotId;

    auto tree = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<Variable>("inputVar"),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    auto env = VariableEnvironment::build(tree);
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);

    {
        inputAccessor.reset(sbe::value::TypeTags::NumberDouble,
                            sbe::value::bitcastFrom<double>(123.0));
        auto result = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT(result);
    }

    {
        auto [tag, val] = sbe::value::makeNewString("123");
        inputAccessor.reset(tag, val);
        auto result = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT(!result);
    }
}

TEST_F(ABTSBE, LowerComparisonCollation) {
    sbe::value::OwnedValueAccessor lhsAccessor;
    sbe::value::OwnedValueAccessor rhsAccessor;
    auto lhsSlotId = bindAccessor(&lhsAccessor);
    auto rhsSlotId = bindAccessor(&rhsAccessor);
    SlotVarMap map;
    map["lhs"] = lhsSlotId;
    map["rhs"] = rhsSlotId;

    sbe::InputParamToSlotMap inputParamToSlotMap;

    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    registerSlot("collator"_sd,
                 sbe::value::TypeTags::collator,
                 sbe::value::bitcastFrom<const CollatorInterface*>(&collator),
                 false);

    auto tree = make<BinaryOp>(Operations::Cmp3w, make<Variable>("lhs"), make<Variable>("rhs"));
    auto env = VariableEnvironment::build(tree);
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);
    auto compiledExpr = compileExpression(*expr);

    auto checkCmp3w = [&](StringData lhs, StringData rhs, int result) {
        auto [lhsTag, lhsValue] = sbe::value::makeNewString(lhs);
        lhsAccessor.reset(true, lhsTag, lhsValue);
        auto [rhsTag, rhsValue] = sbe::value::makeNewString(rhs);
        rhsAccessor.reset(true, rhsTag, rhsValue);

        auto [tag, value] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(tag, value);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt32, tag);
        ASSERT_EQ(result, sbe::value::bitcastTo<int32_t>(value))
            << "comparing string '" << lhs << "' and '" << rhs << "'";
    };

    checkCmp3w("ABC", "abc", 0);
    checkCmp3w("aCC", "abb", 1);
    checkCmp3w("AbX", "aBy", -1);
}

// The following nullability tests verify that ConstEval, which performs rewrites and
// simplifications based on the nullability value of expressions, does not change the result of the
// evaluation of And and Or. eval(E) == eval(constEval(E))

TEST_F(ABTSBE, NonNullableLhsOrTrueConstFold) {
    // E = non-nullable lhs (resolvable variable) || true
    // eval(E) == eval(constEval(E))
    auto tree = _binary("Or", _binary("Gt", "x"_var, "5"_cint32), _cbool(true))._n;
    auto treeConstFold = constFold(tree);

    auto var =
        std::make_pair(ProjectionName{"x"_sd}, sbe::value::makeValue(mongo::Value((int32_t)1)));

    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NonNullableLhsOrFalseConstFold) {
    // E = non-nullable lhs (resolvable variable) || false
    // eval(E) == eval(constEval(E))
    auto tree = _binary("Or", _binary("Gt", "x"_var, "5"_cint32), _cbool(false))._n;
    auto treeConstFold = constFold(tree);

    auto var =
        std::make_pair(ProjectionName{"x"_sd}, sbe::value::makeValue(mongo::Value((int32_t)1)));

    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NullableLhsOrTrueConstFold) {
    // E = nullable lhs (Nothing) || true
    // eval(E) == eval(constEval(E))
    auto tree = _binary("Or", _cnothing(), _cbool(true))._n;
    auto treeConstFold = constFold(tree);

    auto var = boost::none;
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NullableLhsOrFalseConstFold) {
    // E = nullable lhs (Nothing) || false
    // eval(E) == eval(constEval(E))
    auto tree = _binary("Or", _cnothing(), _cbool(false))._n;
    auto treeConstFold = constFold(tree);

    auto var = boost::none;
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NonNullableLhsAndFalseConstFold) {
    // E = non-nullable lhs (resolvable variable) && false
    // eval(E) == eval(constEval(E))
    auto tree = _binary("And", _binary("Gt", "x"_var, "5"_cint32), _cbool(false))._n;
    auto treeConstFold = constFold(tree);

    auto var =
        std::make_pair(ProjectionName{"x"_sd}, sbe::value::makeValue(mongo::Value((int32_t)1)));
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NonNullableLhsAndTrueConstFold) {
    // E = non-nullable lhs (resolvable variable) && true
    // eval(E) == eval(constEval(E))
    auto tree = _binary("And", _binary("Gt", "x"_var, "5"_cint32), _cbool(true))._n;
    auto treeConstFold = constFold(tree);

    auto var =
        std::make_pair(ProjectionName{"x"_sd}, sbe::value::makeValue(mongo::Value((int32_t)1)));
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NullableLhsAndFalseConstFold) {
    // E = nullable lhs (Nothing) && false
    // eval(E) == eval(constEval(E))
    auto tree = _binary("And", _cnothing(), _cbool(false))._n;
    auto treeConstFold = constFold(tree);

    auto var = boost::none;
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(ABTSBE, NullableLhsAndTrueConstFold) {
    // E = nullable lhs (Nothing) && true
    // eval(E) == eval(constEval(E))
    auto tree = _binary("And", _cnothing(), _cbool(true))._n;
    auto treeConstFold = constFold(tree);

    auto var = boost::none;
    auto res = evalExpr(tree, var);
    auto resConstFold = evalExpr(treeConstFold, var);

    assertEqualValues(res, resConstFold);
}

TEST_F(NodeSBE, Lower1) {
    auto prefixId = PrefixId::createForTests();
    Metadata metadata{{}};

    auto opCtx = makeOperationContext();
    auto pipeline = parsePipeline("[{$project:{'a.b.c.d':{$literal:'abc'}}}]",
                                  NamespaceString::createNamespaceString_forTest("test"),
                                  opCtx.get());

    const auto [tag, val] = sbe::value::makeNewArray();
    {
        // Create an array of array with one RecordId and one empty document.
        auto outerArrayPtr = sbe::value::getArrayView(val);

        const auto [tag1, val1] = sbe::value::makeNewArray();
        auto innerArrayPtr = sbe::value::getArrayView(val1);

        const auto [recordTag, recordVal] = sbe::value::makeNewRecordId(0);
        innerArrayPtr->push_back(recordTag, recordVal);

        const auto [tag2, val2] = sbe::value::makeNewObject();
        innerArrayPtr->push_back(tag2, val2);

        outerArrayPtr->push_back(tag1, val1);
    }
    ABT valueArray = make<Constant>(tag, val);

    const ProjectionName scanProjName = prefixId.getNextId("scan");
    QueryParameterMap qp;
    ABT tree = translatePipelineToABT(metadata,
                                      *pipeline.get(),
                                      scanProjName,
                                      make<ValueScanNode>(ProjectionNameVector{scanProjName},
                                                          boost::none,
                                                          std::move(valueArray),
                                                          true /*hasRID*/),
                                      prefixId,
                                      qp);

    auto phaseManager = makePhaseManager(OptPhaseManager::getAllProdRewrites(),
                                         prefixId,
                                         {{{"test", createScanDef({}, {})}}},
                                         boost::none /*costModel*/,
                                         DebugInfo::kDefaultForTests);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(tree));
    auto env = VariableEnvironment::build(planAndProps._node);
    SlotVarMap map;
    auto runtimeEnv = std::make_unique<sbe::RuntimeEnvironment>();
    boost::optional<sbe::value::SlotId> ridSlot;
    sbe::value::SlotIdGenerator ids;
    sbe::InputParamToSlotMap inputParamToSlotMap;

    SBENodeLowering g{
        env, *runtimeEnv, ids, inputParamToSlotMap, phaseManager.getMetadata(), planAndProps._map};
    auto sbePlan = g.optimize(planAndProps._node, map, ridSlot);
    ASSERT_EQ(1, map.size());
    ASSERT_FALSE(ridSlot);

    sbe::CompileCtx ctx(std::move(runtimeEnv));
    sbePlan->prepare(ctx);

    std::vector<sbe::value::SlotAccessor*> accessors;
    for (auto& [name, slot] : map) {
        std::cout << name << " ";
        accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
    }
    std::cout << "\n";
    sbePlan->attachToOperationContext(opCtx.get());
    sbePlan->open(false);
    while (sbePlan->getNext() != sbe::PlanState::IS_EOF) {
        for (auto acc : accessors) {
            std::cout << acc->getViewOfValue() << " ";
        }
        std::cout << "\n";
    };
    sbePlan->close();
}


TEST_F(NodeSBE, Lower2) {
    using namespace properties;

    const auto [arrTag, arrVal] = sbe::value::makeNewArray();
    sbe::value::Array* arr = sbe::value::getArrayView(arrVal);
    for (int i = 1; i < 4; i++) {
        arr->push_back(sbe::value::TypeTags::NumberInt32, i);
    }
    ABT arrayConst = make<Constant>(arrTag, arrVal);

    // Test lowering of a SortedMerge node.
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("EqMember", ExprHolder{arrayConst}))),
                                  "root"_var))
                   .finish(_scan("root", "test"));

    // Optimize the logical plan.
    // We have to fake some metadata for this to work.
    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test",
           createScanDef({},
                         {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false)},
                          {"index2", makeIndexDefinition("b", CollationOp::Ascending, false)}})}}},
        boost::none /*costModel*/,
        DebugInfo::kDefaultForTests);

    phaseManager.optimize(root);

    // Now we should have a plan with a SortedMerge in it.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, test]\n"
        "SortedMerge []\n"
        "|   |   |   |   collation: \n"
        "|   |   |   |       rid_0: Ascending\n"
        "|   |   IndexScan [{'<rid>': rid_0}, scanDefName: test, indexDefName: index1, interval: "
        "{=Const [3]}]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: test, indexDefName: index1, interval: "
        "{=Const [2]}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: test, indexDefName: index1, interval: {=Const "
        "[1]}]\n",
        root);

    // TODO SERVER-72010 fix test or SortedMergeNode logic so building VariableEnvironment succeeds

    // Lower to SBE.
    // auto env = VariableEnvironment::build(root);
    // SlotVarMap map;
    // boost::optional<sbe::value::SlotId> ridSlot;
    // sbe::value::SlotIdGenerator ids;
    // SBENodeLowering g{env,
    //                   ids,
    //                   phaseManager.getMetadata(),
    //                   phaseManager.getNodeToGroupPropsMap(),
    //                   false /*randomScan*/};
    // auto sbePlan = g.optimize(root, map, ridSlot, ids);

    // ASSERT_EQ(
    //     "[4] smerge [s4] [asc] [\n"
    //     "    [s1] [s1] [3] ixseek ks(2ll, 0, 1ll, 1ll) ks(2ll, 0, 1ll, 2ll) none s1 none [s2 = 0]
    //     "
    //     "@\"11111111-1111-1111-1111-111111111111\" @\"index1\" true , \n"
    //     "    [s3] [s3] [3] ixseek ks(2ll, 0, 2ll, 1ll) ks(2ll, 0, 2ll, 2ll) none s3 none [] "
    //     "@\"11111111-1111-1111-1111-111111111111\" @\"index2\" true \n"
    //     "] ",
    //     sbe::DebugPrinter().print(*sbePlan.get()));
}

TEST_F(NodeSBE, RequireRID) {
    auto prefixId = PrefixId::createForTests();
    Metadata metadata{{}};

    auto opCtx = makeOperationContext();
    auto pipeline = parsePipeline(
        "[{$match: {a: 2}}]", NamespaceString::createNamespaceString_forTest("test"), opCtx.get());

    const ProjectionName scanProjName = prefixId.getNextId("scan");

    const auto [tag, val] = sbe::value::makeNewArray();
    {
        // Create an array of 10 arrays, each inner array consisting of a RecordId at the first
        // position, followed by a document with the field "a" containing sequential integers from 0
        // to 9.
        auto outerArrayPtr = sbe::value::getArrayView(val);
        for (size_t i = 0; i < 10; i++) {
            const auto [tag1, val1] = sbe::value::makeNewArray();
            auto innerArrayPtr = sbe::value::getArrayView(val1);

            const auto [recordTag, recordVal] = sbe::value::makeNewRecordId(i);
            innerArrayPtr->push_back(recordTag, recordVal);

            const auto [tag2, val2] = sbe::value::makeNewObject();
            auto objPtr = sbe::value::getObjectView(val2);
            objPtr->push_back("a", sbe::value::TypeTags::NumberInt32, i);

            innerArrayPtr->push_back(tag2, val2);
            outerArrayPtr->push_back(tag1, val1);
        }
    }
    ABT valueArray = make<Constant>(tag, val);

    QueryParameterMap qp;
    ABT tree =
        translatePipelineToABT(metadata,
                               *pipeline.get(),
                               scanProjName,
                               make<ValueScanNode>(ProjectionNameVector{scanProjName},
                                                   createInitialScanProps(scanProjName, "test"),
                                                   std::move(valueArray),
                                                   true /*hasRID*/),
                               prefixId,
                               qp);

    auto phaseManager = makePhaseManagerRequireRID(OptPhaseManager::getAllProdRewrites(),
                                                   prefixId,
                                                   {{{"test", createScanDef({}, {})}}},
                                                   DebugInfo::kDefaultForTests);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(tree));
    auto env = VariableEnvironment::build(planAndProps._node);

    SlotVarMap map;
    auto runtimeEnv = std::make_unique<sbe::RuntimeEnvironment>();
    boost::optional<sbe::value::SlotId> ridSlot;
    sbe::value::SlotIdGenerator ids;
    sbe::InputParamToSlotMap inputParamToSlotMap;

    SBENodeLowering g{
        env, *runtimeEnv, ids, inputParamToSlotMap, phaseManager.getMetadata(), planAndProps._map};
    auto sbePlan = g.optimize(planAndProps._node, map, ridSlot);
    ASSERT_EQ(1, map.size());
    ASSERT_TRUE(ridSlot);

    sbe::CompileCtx ctx(std::move(runtimeEnv));
    sbePlan->prepare(ctx);

    std::vector<sbe::value::SlotAccessor*> accessors;
    for (auto& [name, slot] : map) {
        accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
    }
    accessors.emplace_back(sbePlan->getAccessor(ctx, *ridSlot));

    sbePlan->attachToOperationContext(opCtx.get());
    sbePlan->open(false);

    size_t resultSize = 0;
    while (sbePlan->getNext() != sbe::PlanState::IS_EOF) {
        resultSize++;

        // Assert we have one result, and it is equal to {a: 2} with rid = RecordId(2).
        const auto [resultTag, resultVal] = accessors.at(0)->getViewOfValue();
        ASSERT_EQ(sbe::value::TypeTags::Object, resultTag);
        const auto objPtr = sbe::value::getObjectView(resultVal);
        ASSERT_EQ(1, objPtr->size());
        const auto [fieldTag, fieldVal] = objPtr->getField("a");
        ASSERT_EQ(sbe::value::TypeTags::NumberInt32, fieldTag);
        ASSERT_EQ(2, fieldVal);

        const auto [ridTag, ridVal] = accessors.at(1)->getViewOfValue();
        ASSERT_EQ(sbe::value::TypeTags::RecordId, ridTag);
        ASSERT_EQ(2, sbe::value::getRecordIdView(ridVal)->getLong());
    };
    sbePlan->close();

    ASSERT_EQ(1, resultSize);
}

TEST_F(NodeSBE, SamplingTest) {
    auto prefixId = PrefixId::createForTests();
    const std::string scanDefName = "test";
    Metadata metadata{{{scanDefName,
                        createScanDef({},
                                      {{"index1",
                                        makeIndexDefinition(
                                            "a", CollationOp::Ascending, true /*isMultiKey*/)}})}}};

    auto opCtx = makeOperationContext();
    auto pipeline = parsePipeline(
        "[{$match: {a: 2}}]", NamespaceString::createNamespaceString_forTest("test"), opCtx.get());

    const ProjectionName scanProjName = prefixId.getNextId("scan");
    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    ABT tree = translatePipelineToABT(metadata,
                                      *pipeline.get(),
                                      scanProjName,
                                      make<ScanNode>(scanProjName, scanDefName),
                                      prefixId,
                                      qp);

    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{{{OptPhase::MemoSubstitutionPhase,
                                              OptPhase::MemoExplorationPhase,
                                              OptPhase::MemoImplementationPhase},
                                             kDefaultExplorationSet,
                                             kDefaultSubstitutionSet},
                                            prefixId,
                                            false /*requireRID*/,
                                            metadata,
                                            makeHeuristicCE(),
                                            makeHeuristicCE(),
                                            makeCostEstimator(getTestCostModel()),
                                            defaultConvertPathToInterval,
                                            defaultConvertPathToInterval,
                                            DebugInfo::kDefaultForProd,
                                            {._sqrtSampleSizeEnabled = false},
                                            qp,
                                            optCounterInfo};

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager{
        {{OptPhase::MemoSubstitutionPhase,
          OptPhase::MemoExplorationPhase,
          OptPhase::MemoImplementationPhase},
         kDefaultExplorationSet,
         kDefaultSubstitutionSet},
        prefixId,
        false /*requireRID*/,
        metadata,
        std::make_unique<ce::SamplingEstimator>(std::move(phaseManagerForSampling),
                                                1000 /*collectionSize*/,
                                                DebugInfo::kDefaultForTests,
                                                prefixId,
                                                makeHeuristicCE(),
                                                std::make_unique<ABTRecorder>(nodes)),
        makeHeuristicCE(),
        makeCostEstimator(getTestCostModel()),
        defaultConvertPathToInterval,
        ConstEval::constFold,
        DebugInfo::kDefaultForTests,
        {} /*queryHints*/,
        qp,
        optCounterInfo};

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(tree));

    ASSERT_EQ(1, nodes.size());

    // We have a single plan to sample the predicate
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_1]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': evalTemp_1}, test]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, test]\n",
        nodes.front());
}

/**
 * This transport is used to populate default values into the NodeToGroupProps map to get around the
 * fact that the plan was not obtained from the memo. At this point we are interested only in the
 * planNodeIds being distinct.
 */
class PropsTransport {
public:
    template <typename T, typename... Ts>
    void transport(const T& node, NodeToGroupPropsMap& propMap, Ts&&...) {
        if constexpr (std::is_base_of_v<Node, T>) {
            propMap.emplace(&node,
                            NodeProps{_planNodeId++,
                                      {-1, 0} /*groupId*/,
                                      {} /*logicalProps*/,
                                      {} /*physicalProps*/,
                                      boost::none /*ridProjName*/,
                                      CostType::kZero /*cost*/,
                                      CostType::kZero /*localCost*/,
                                      0.0 /*adjustedCE*/});
        }
    }

    void updatePropsMap(const ABT& n, NodeToGroupPropsMap& propMap) {
        algebra::transport<false>(n, *this, propMap);
    }

private:
    int32_t _planNodeId = 0;
};

TEST_F(NodeSBE, SpoolFibonacci) {
    using namespace unit_test_abt_literals;

    auto prefixId = PrefixId::createForTests();
    Metadata metadata{{}};

    // Construct a spool-based recursive plan to compute the first 10 Fibonacci numbers. The main
    // plan (first child of the union) sets up the initial conditions (val = 1, prev = 0, and it =
    // 1), and the recursive subplan is computing the actual Fibonacci sequence and ensures we
    // terminate after 10 numbers.
    auto recursion =
        NodeBuilder{}
            .eval("val", _binary("Add", "valIn"_var, "valIn_prev"_var))
            .eval("val_prev", "valIn"_var)
            .eval("it", _binary("Add", "itIn"_var, "1"_cint64))
            .filter(_binary("Lt", "itIn"_var, "10"_cint64))
            .finish(_spoolc("Stack", 1 /*spoolId*/, _varnames("valIn", "valIn_prev", "itIn")));

    auto tree = NodeBuilder{}
                    .root("val")
                    .spoolp("Lazy", 1 /*spoolId*/, _varnames("val", "val_prev", "it"), _cbool(true))
                    .un(_varnames("val", "val_prev", "it"), {NodeHolder{std::move(recursion)}})
                    .eval("val", "1"_cint64)
                    .eval("val_prev", "0"_cint64)
                    .eval("it", "1"_cint64)
                    .ls(1, 0)
                    .finish(_coscan());

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{val}]\n"
        "SpoolProducer [Lazy, id: 1, {it, val, val_prev}]\n"
        "|   |   Const [true]\n"
        "Union [{it, val, val_prev}]\n"
        "|   Evaluation [{val}]\n"
        "|   |   BinaryOp [Add]\n"
        "|   |   |   Variable [valIn_prev]\n"
        "|   |   Variable [valIn]\n"
        "|   Evaluation [{val_prev} = Variable [valIn]]\n"
        "|   Evaluation [{it}]\n"
        "|   |   BinaryOp [Add]\n"
        "|   |   |   Const [1]\n"
        "|   |   Variable [itIn]\n"
        "|   Filter []\n"
        "|   |   BinaryOp [Lt]\n"
        "|   |   |   Const [10]\n"
        "|   |   Variable [itIn]\n"
        "|   SpoolConsumer [Stack, id: 1, {itIn, valIn, valIn_prev}]\n"
        "Evaluation [{val} = Const [1]]\n"
        "Evaluation [{val_prev} = Const [0]]\n"
        "Evaluation [{it} = Const [1]]\n"
        "LimitSkip [limit: 1, skip: 0]\n"
        "CoScan []\n",
        tree);

    NodeToGroupPropsMap props;
    PropsTransport{}.updatePropsMap(tree, props);

    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    auto runtimeEnv = std::make_unique<sbe::RuntimeEnvironment>();
    boost::optional<sbe::value::SlotId> ridSlot;
    sbe::value::SlotIdGenerator ids;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    SBENodeLowering g{env, *runtimeEnv, ids, inputParamToSlotMap, metadata, props};
    auto sbePlan = g.optimize(tree, map, ridSlot);
    ASSERT_EQ(1, map.size());

    auto opCtx = makeOperationContext();
    sbe::CompileCtx ctx(std::move(runtimeEnv));
    sbePlan->prepare(ctx);

    std::vector<sbe::value::SlotAccessor*> accessors;
    for (auto& [name, slot] : map) {
        accessors.emplace_back(sbePlan->getAccessor(ctx, slot));
    }

    sbePlan->attachToOperationContext(opCtx.get());
    sbePlan->open(false);

    std::vector<int64_t> results;
    while (sbePlan->getNext() != sbe::PlanState::IS_EOF) {
        const auto [resultTag, resultVal] = accessors.front()->getViewOfValue();
        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        results.push_back(resultVal);
    };
    sbePlan->close();

    // Verify we are getting 10 Fibonacci numbers.
    ASSERT_EQ(10, results.size());

    ASSERT_EQ(1, results.at(0));
    ASSERT_EQ(1, results.at(1));
    for (size_t i = 2; i < 10; i++) {
        ASSERT_EQ(results.at(i), results.at(i - 1) + results.at(i - 2));
    }
}

}  // namespace
}  // namespace mongo::optimizer
