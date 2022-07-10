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

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/sbe_abt_test_util.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

TEST_F(ABTSBE, Lower1) {
    auto tree = Constant::int64(100);
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

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
        makeSeq(
            make<Constant>(tagArr, valArr),
            make<LambdaAbstraction>(
                "x", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(10)))));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);
}

TEST_F(ABTSBE, Lower6) {
    PrefixId prefixId;

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
        make<PathField>("fieldA",
                        make<PathTraverse>(
                            make<PathComposeM>(
                                make<PathField>("fieldB", make<PathDefault>(Constant::int64(0))),
                                make<PathField>("fieldC", make<PathConstant>(Constant::int64(50)))),
                            PathTraverse::kUnlimited)),
        make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathLowering{prefixId, env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    // std::cout << ExplainGenerator::explain(tree);

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);

    // std::cout << std::pair{resultTag, resultVal} << "\n";

    ASSERT_EQ(sbe::value::TypeTags::Object, resultTag);
}

TEST_F(ABTSBE, Lower7) {
    PrefixId prefixId;

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
                      make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(2)),
                                         PathTraverse::kSingleLevel)),
        make<Variable>("root"));

    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathLowering{prefixId, env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);

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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);
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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);
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

    auto expr = SBEExpressionLowering{env, map}.optimize(tree);
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

TEST_F(NodeSBE, Lower1) {
    PrefixId prefixId;
    Metadata metadata{{}};

    OperationContextNoop noop;
    auto pipeline =
        parsePipeline("[{$project:{'a.b.c.d':{$literal:'abc'}}}]", NamespaceString("test"), &noop);

    const auto [tag, val] = sbe::value::makeNewArray();
    {
        // Create an array of array with one empty document.
        auto outerArrayPtr = sbe::value::getArrayView(val);

        const auto [tag1, val1] = sbe::value::makeNewArray();
        auto innerArrayPtr = sbe::value::getArrayView(val1);

        const auto [tag2, val2] = sbe::value::makeNewObject();
        innerArrayPtr->push_back(tag2, val2);

        outerArrayPtr->push_back(tag1, val1);
    }
    ABT tree = make<Constant>(tag, val);

    const ProjectionName scanProjName = prefixId.getNextId("scan");
    tree = translatePipelineToABT(
        metadata,
        *pipeline.get(),
        scanProjName,
        make<ValueScanNode>(ProjectionNameVector{scanProjName}, std::move(tree)),
        prefixId);

    OptPhaseManager phaseManager(
        OptPhaseManager::getAllRewritesSet(), prefixId, {{}}, DebugInfo::kDefaultForTests);

    ASSERT_TRUE(phaseManager.optimize(tree));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::value::SlotIdGenerator ids;

    SBENodeLowering g{env,
                      map,
                      ids,
                      phaseManager.getMetadata(),
                      phaseManager.getNodeToGroupPropsMap(),
                      phaseManager.getRIDProjections()};

    auto sbePlan = g.optimize(tree);

    auto opCtx = makeOperationContext();

    sbe::CompileCtx ctx(std::make_unique<sbe::RuntimeEnvironment>());
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

}  // namespace
}  // namespace mongo::optimizer
