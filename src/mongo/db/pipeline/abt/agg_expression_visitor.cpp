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

#include <absl/container/node_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/expr_algebrizer_context.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::optimizer {

class ABTAggExpressionVisitor final : public ExpressionConstVisitor {
public:
    ABTAggExpressionVisitor(ExpressionAlgebrizerContext& ctx) : _ctx(ctx){};

    void visit(const ExpressionConstant* expr) final {
        auto [tag, val] = sbe::value::makeValue(expr->getValue());
        _ctx.push<Constant>(tag, val);
    }

    void visit(const ExpressionAbs* expr) final {
        pushSingleArgFunctionFromTop("abs");
    }

    void visit(const ExpressionAdd* expr) final {
        pushArithmeticBinaryExpr(expr, Operations::Add);
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, Operations::And);
    }

    void visit(const ExpressionAnyElementTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionArray* expr) final {
        const size_t childCount = expr->getChildren().size();
        _ctx.ensureArity(childCount);

        // Need to process in reverse order because of the stack.
        ABTVector args;
        for (size_t i = 0; i < childCount; i++) {
            args.emplace_back(_ctx.pop());
        }

        std::reverse(args.begin(), args.end());
        _ctx.push<FunctionCall>("newArray", std::move(args));
    }

    void visit(const ExpressionArrayElemAt* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitAnd* expr) final {
        unsupportedExpression("bitAnd");
    }
    void visit(const ExpressionBitOr* expr) final {
        unsupportedExpression("bitOr");
    }
    void visit(const ExpressionBitXor* expr) final {
        unsupportedExpression("bitXor");
    }
    void visit(const ExpressionBitNot* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFirst* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLast* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionObjectToArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionArrayToObject* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionBsonSize* expr) final {
        pushSingleArgFunctionFromTop("bsonSize");
    }

    void visit(const ExpressionCeil* expr) final {
        pushSingleArgFunctionFromTop("ceil");
    }

    void visit(const ExpressionCoerceToBool* expr) final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }

    void visit(const ExpressionCompare* expr) final {
        _ctx.ensureArity(2);
        ABT right = _ctx.pop();
        ABT left = _ctx.pop();

        const auto translateCmpOpFn = [](const ExpressionCompare::CmpOp op) {
            switch (op) {
                case ExpressionCompare::CmpOp::EQ:
                    return Operations::Eq;

                case ExpressionCompare::CmpOp::NE:
                    return Operations::Neq;

                case ExpressionCompare::CmpOp::GT:
                    return Operations::Gt;

                case ExpressionCompare::CmpOp::GTE:
                    return Operations::Gte;

                case ExpressionCompare::CmpOp::LT:
                    return Operations::Lt;

                case ExpressionCompare::CmpOp::LTE:
                    return Operations::Lte;

                case ExpressionCompare::CmpOp::CMP:
                    return Operations::Cmp3w;

                default:
                    MONGO_UNREACHABLE;
            }
        };

        const auto addEvalFilterFn = [&](ABT path, ABT expr, const Operations op) {
            PathAppender::appendInPlace(path, make<PathCompare>(op, std::move(expr)));

            _ctx.push<EvalFilter>(std::move(path), _ctx.getRootProjVar());
        };

        const Operations op = translateCmpOpFn(expr->getOp());
        if (op != Operations::Cmp3w) {
            // TODO: SERVER-67306. Remove requirement that path is simple.
            // Then we can re-use traverse between EvalPath/EvalFilter contexts.

            // If we have simple EvalPaths coming from the left or on the right, add a PathCompare,
            // and keep propagating the path.
            if (auto leftPtr = left.cast<EvalPath>(); leftPtr != nullptr &&
                isSimplePath(leftPtr->getPath()) && leftPtr->getInput() == _ctx.getRootProjVar()) {
                addEvalFilterFn(std::move(leftPtr->getPath()), std::move(right), op);
                return;
            }
            if (auto rightPtr = right.cast<EvalPath>(); rightPtr != nullptr &&
                isSimplePath(rightPtr->getPath()) &&
                rightPtr->getInput() == _ctx.getRootProjVar()) {
                addEvalFilterFn(
                    std::move(rightPtr->getPath()), std::move(left), flipComparisonOp(op));
                return;
            }
        }
        _ctx.push<BinaryOp>(op, std::move(left), std::move(right));
    }

    void visit(const ExpressionConcat* expr) final {
        pushMultiArgFunctionFromTop("concat", expr->getChildren().size());
    }

    void visit(const ExpressionConcatArrays* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionCond* expr) final {
        _ctx.ensureArity(3);
        ABT cond = _ctx.pop();
        ABT thenCase = _ctx.pop();
        ABT elseCase = _ctx.pop();
        _ctx.push<If>(std::move(cond), std::move(thenCase), std::move(elseCase));
    }

    void visit(const ExpressionDateFromString* expr) final {
        unsupportedExpression("$dateFromString");
    }

    void visit(const ExpressionDateFromParts* expr) final {
        unsupportedExpression("$dateFromParts");
    }

    void visit(const ExpressionDateDiff* expr) final {
        unsupportedExpression("$dateDiff");
    }

    void visit(const ExpressionDateToParts* expr) final {
        unsupportedExpression("$dateToParts");
    }

    void visit(const ExpressionDateToString* expr) final {
        unsupportedExpression("$dateToString");
    }

    void visit(const ExpressionDateTrunc* expr) final {
        unsupportedExpression("$dateTrunc");
    }

    void visit(const ExpressionDivide* expr) final {
        pushArithmeticBinaryExpr(expr, Operations::Div);
    }

    void visit(const ExpressionExp* expr) final {
        pushSingleArgFunctionFromTop("exp");
    }

    void visit(const ExpressionFieldPath* expr) final {
        const auto& varId = expr->getVariableId();
        if (Variables::isUserDefinedVariable(varId)) {
            _ctx.push<Variable>(generateVariableName(varId));
            return;
        }

        const FieldPath& fieldPath = expr->getFieldPath();
        const size_t pathLength = fieldPath.getPathLength();
        if (pathLength < 1) {
            return;
        }

        const auto& firstFieldName = fieldPath.getFieldName(0);
        if (pathLength == 1 && firstFieldName == "ROOT") {
            _ctx.push<Variable>(_ctx.getRootProjection());
            return;
        }
        uassert(6624239, "Unexpected leading path element.", firstFieldName == "CURRENT");

        // Here we skip over "CURRENT" first path element. This is represented by rootProjection
        // variable.
        ABT path = translateFieldPath(
            fieldPath,
            make<PathIdentity>(),
            [](FieldNameType fieldName, const bool isLastElement, ABT input) {
                if (!isLastElement) {
                    input = make<PathTraverse>(PathTraverse::kUnlimited, std::move(input));
                }
                return make<PathGet>(std::move(fieldName), std::move(input));
            },
            1ul);

        _ctx.push<EvalPath>(std::move(path), make<Variable>(_ctx.getRootProjection()));
    }

    void visit(const ExpressionFilter* expr) final {
        const auto& varId = expr->getVariableId();
        uassert(6624427,
                "Filter variable must be user-defined.",
                Variables::isUserDefinedVariable(varId));
        const ProjectionName varName{generateVariableName(varId)};

        _ctx.ensureArity(2);
        ABT filter = _ctx.pop();
        ABT input = _ctx.pop();

        _ctx.push<EvalPath>(
            make<PathTraverse>(
                PathTraverse::kUnlimited,
                make<PathLambda>(make<LambdaAbstraction>(
                    varName,
                    make<If>(std::move(filter), make<Variable>(varName), Constant::nothing())))),
            std::move(input));
    }

    void visit(const ExpressionFloor* expr) final {
        pushSingleArgFunctionFromTop("floor");
    }

    void visit(const ExpressionIfNull* expr) final {
        pushMultiArgFunctionFromTop("ifNull", 2);
    }

    void visit(const ExpressionIn* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIsNumber* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLet* expr) final {
        unsupportedExpression("$let");
    }

    void visit(const ExpressionLn* expr) final {
        pushSingleArgFunctionFromTop("ln");
    }

    void visit(const ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLog10* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionInternalFLEEqual* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionInternalFLEBetween* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }

    void visit(const ExpressionMeta* expr) final {
        unsupportedExpression("$meta");
    }

    void visit(const ExpressionMod* expr) final {
        pushMultiArgFunctionFromTop("mod", 2);
    }

    void visit(const ExpressionMultiply* expr) final {
        pushArithmeticBinaryExpr(expr, Operations::Mult);
    }

    void visit(const ExpressionNot* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionObject* expr) final {
        const auto& expressions = expr->getChildExpressions();
        const size_t childCount = expressions.size();
        _ctx.ensureArity(childCount);

        // Need to process in reverse order because of the stack.
        ABTVector children;
        for (size_t i = 0; i < childCount; i++) {
            children.emplace_back(_ctx.pop());
        }

        sbe::value::Object object;
        for (size_t i = 0; i < childCount; i++) {
            ABT& child = children.at(childCount - i - 1);
            uassert(
                6624345, "Only constants are supported as object fields.", child.is<Constant>());

            auto [tag, val] = child.cast<Constant>()->get();
            // Copy the value before inserting into the object
            auto [tagCopy, valCopy] = sbe::value::copyValue(tag, val);
            object.push_back(expressions.at(i).first, tagCopy, valCopy);
        }

        auto [tag, val] = sbe::value::makeCopyObject(object);
        _ctx.push<Constant>(tag, val);
    }

    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, Operations::Or);
    }

    void visit(const ExpressionPow* expr) final {
        unsupportedExpression("$pow");
    }

    void visit(const ExpressionRange* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }

    void visit(const ExpressionReplaceOne* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetDifference* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetEquals* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetIntersection* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetIsSubset* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetUnion* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSize* expr) final {
        pushSingleArgFunctionFromTop("getArraySize");
    }

    void visit(const ExpressionReverseArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSortArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIsArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionRound* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSplit* expr) final {
        pushMultiArgFunctionFromTop("split", expr->getChildren().size());
    }

    void visit(const ExpressionSqrt* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrcasecmp* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubstrBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubstrCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrLenBytes* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrLenCP* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubtract* expr) final {
        pushArithmeticBinaryExpr(expr, Operations::Sub);
    }

    void visit(const ExpressionSwitch* expr) final {
        const size_t arity = expr->getChildren().size();
        _ctx.ensureArity(arity);
        const size_t numCases = (arity - 1) / 2;

        ABTVector children;
        for (size_t i = 0; i < numCases; i++) {
            children.emplace_back(_ctx.pop());
            children.emplace_back(_ctx.pop());
        }

        if (expr->getChildren().back() != nullptr) {
            children.push_back(_ctx.pop());
        }

        _ctx.push<FunctionCall>("switch", std::move(children));
    }

    void visit(const ExpressionTestApiVersion* expr) final {
        unsupportedExpression("$_testApiVersion");
    }

    void visit(const ExpressionToLower* expr) final {
        pushSingleArgFunctionFromTop("toLower");
    }

    void visit(const ExpressionToUpper* expr) final {
        pushSingleArgFunctionFromTop("toUpper");
    }

    void visit(const ExpressionTrim* expr) final {
        unsupportedExpression("$trim");
    }

    void visit(const ExpressionTrunc* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }

    void visit(const ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }

    void visit(const ExpressionRegexFind* expr) final {
        unsupportedExpression("$regexFind");
    }

    void visit(const ExpressionRegexFindAll* expr) final {
        unsupportedExpression("$regexFindAll");
    }

    void visit(const ExpressionRegexMatch* expr) final {
        unsupportedExpression("$regexMatch");
    }

    void visit(const ExpressionCosine* expr) final {
        pushSingleArgFunctionFromTop("cosine");
    }

    void visit(const ExpressionSine* expr) final {
        pushSingleArgFunctionFromTop("sine");
    }

    void visit(const ExpressionTangent* expr) final {
        pushSingleArgFunctionFromTop("tangent");
    }

    void visit(const ExpressionArcCosine* expr) final {
        pushSingleArgFunctionFromTop("arcCosine");
    }

    void visit(const ExpressionArcSine* expr) final {
        pushSingleArgFunctionFromTop("arcSine");
    }

    void visit(const ExpressionArcTangent* expr) final {
        pushSingleArgFunctionFromTop("arcTangent");
    }

    void visit(const ExpressionArcTangent2* expr) final {
        pushSingleArgFunctionFromTop("arcTangent2");
    }

    void visit(const ExpressionHyperbolicArcTangent* expr) final {
        pushSingleArgFunctionFromTop("arcTangentH");
    }

    void visit(const ExpressionHyperbolicArcCosine* expr) final {
        pushSingleArgFunctionFromTop("arcCosineH");
    }

    void visit(const ExpressionHyperbolicArcSine* expr) final {
        pushSingleArgFunctionFromTop("arcSineH");
    }

    void visit(const ExpressionHyperbolicTangent* expr) final {
        pushSingleArgFunctionFromTop("tangentH");
    }

    void visit(const ExpressionHyperbolicCosine* expr) final {
        pushSingleArgFunctionFromTop("cosineH");
    }

    void visit(const ExpressionHyperbolicSine* expr) final {
        pushSingleArgFunctionFromTop("sineH");
    }

    void visit(const ExpressionDegreesToRadians* expr) final {
        pushSingleArgFunctionFromTop("degreesToRadians");
    }

    void visit(const ExpressionRadiansToDegrees* expr) final {
        pushSingleArgFunctionFromTop("radiansToDegrees");
    }

    void visit(const ExpressionDayOfMonth* expr) final {
        unsupportedExpression("$dayOfMonth");
    }

    void visit(const ExpressionDayOfWeek* expr) final {
        unsupportedExpression("$dayOfWeek");
    }

    void visit(const ExpressionDayOfYear* expr) final {
        unsupportedExpression("$dayOfYear");
    }

    void visit(const ExpressionHour* expr) final {
        unsupportedExpression("$hour");
    }

    void visit(const ExpressionMillisecond* expr) final {
        unsupportedExpression("$millisecond");
    }

    void visit(const ExpressionMinute* expr) final {
        unsupportedExpression("$minute");
    }

    void visit(const ExpressionMonth* expr) final {
        unsupportedExpression("$month");
    }

    void visit(const ExpressionSecond* expr) final {
        unsupportedExpression("$second");
    }

    void visit(const ExpressionWeek* expr) final {
        unsupportedExpression("$week");
    }

    void visit(const ExpressionIsoWeekYear* expr) final {
        unsupportedExpression("$isoWeekYear");
    }

    void visit(const ExpressionIsoDayOfWeek* expr) final {
        unsupportedExpression("$isoDayOfWeek");
    }

    void visit(const ExpressionIsoWeek* expr) final {
        unsupportedExpression("$isoWeek");
    }

    void visit(const ExpressionYear* expr) final {
        unsupportedExpression("$year");
    }

    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }

    void visit(const ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }

    void visit(const ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }

    void visit(const ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }

    void visit(const ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(const ExpressionRandom* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionToHashedIndexKey* expr) final {
        unsupportedExpression("$toHashedIndexKey");
    }

    void visit(const ExpressionDateAdd* expr) final {
        unsupportedExpression("dateAdd");
    }

    void visit(const ExpressionDateSubtract* expr) final {
        unsupportedExpression("dateSubtract");
    }

    void visit(const ExpressionSetField* expr) final {
        unsupportedExpression("$setField");
    }

    void visit(const ExpressionGetField* expr) final {
        unsupportedExpression("$getField");
    }

    void visit(const ExpressionTsSecond* expr) final {
        unsupportedExpression("tsSecond");
    }

    void visit(const ExpressionTsIncrement* expr) final {
        unsupportedExpression("tsIncrement");
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        unsupportedExpression("$_internalOwningShard");
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        unsupportedExpression("$_internalIndexKey");
    }

    void visit(const ExpressionInternalKeyStringValue* expr) final {
        unsupportedExpression(expr->getOpName());
    }

private:
    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(const Expression* expr, Operations logicOp) {
        const bool isAnd = logicOp == Operations::And;
        invariant(isAnd || logicOp == Operations::Or);
        const size_t arity = expr->getChildren().size();
        _ctx.ensureArity(arity);

        if (arity == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value: true
            // and false, respectively.
            _ctx.push(Constant::boolean(isAnd));
            return;
        }

        bool allFilters = true;
        ABTVector children;
        ABTVector childPaths;

        for (size_t i = 0; i < arity; i++) {
            // TODO: SERVER-67306. Remove requirement that path is simple.
            // Then we can re-use traverse between EvalPath/EvalFilter contexts.

            ABT child = _ctx.pop();
            if (auto filterPtr = child.cast<EvalFilter>(); allFilters && filterPtr != nullptr &&
                isSimplePath(filterPtr->getPath()) &&
                filterPtr->getInput() == _ctx.getRootProjVar()) {
                childPaths.push_back(filterPtr->getPath());
            } else {
                allFilters = false;
            }
            children.push_back(std::move(child));
        }

        if (allFilters) {
            // If all children are simple paths, place a path composition.
            ABT result = make<PathIdentity>();
            if (isAnd) {
                for (ABT& child : childPaths) {
                    maybeComposePath<PathComposeM>(result, std::move(child));
                }
            } else {
                for (ABT& child : childPaths) {
                    maybeComposePath<PathComposeA>(result, std::move(child));
                }
            }
            _ctx.push<EvalFilter>(std::move(result), _ctx.getRootProjVar());
        } else {
            ABT result = std::move(children.front());
            for (size_t i = 1; i < arity; i++) {
                result = make<BinaryOp>(logicOp, std::move(result), std::move(children.at(i)));
            }
            _ctx.push(std::move(result));
        }
    }

    void pushMultiArgFunctionFromTop(const std::string& functionName, const size_t argCount) {
        _ctx.ensureArity(argCount);

        ABTVector children;
        for (size_t i = 0; i < argCount; i++) {
            children.emplace_back(_ctx.pop());
        }
        std::reverse(children.begin(), children.end());

        _ctx.push<FunctionCall>(functionName, children);
    }

    void pushSingleArgFunctionFromTop(const std::string& functionName) {
        pushMultiArgFunctionFromTop(functionName, 1);
    }

    void pushArithmeticBinaryExpr(const Expression* expr, const Operations op) {
        const size_t arity = expr->getChildren().size();
        _ctx.ensureArity(arity);

        ABT current = _ctx.pop();
        for (size_t i = 0; i < arity - 1; i++) {
            current = make<BinaryOp>(op, _ctx.pop(), std::move(current));
        }
        _ctx.push(std::move(current));
    }

    ProjectionName generateVariableName(const Variables::Id varId) {
        return ProjectionName{std::string(str::stream() << "var_" << varId)};
    }

    void unsupportedExpression(const char* op) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Expression is not supported: " << op);
    }

    // We don't own this.
    ExpressionAlgebrizerContext& _ctx;
};

class AggExpressionWalker final {
public:
    AggExpressionWalker(ABTAggExpressionVisitor* visitor) : _visitor{visitor} {}

    void postVisit(const Expression* expr) {
        expr->acceptVisitor(_visitor);
    }

private:
    ABTAggExpressionVisitor* _visitor;
};

ABT generateAggExpression(const Expression* expr,
                          const ProjectionName& rootProjection,
                          PrefixId& prefixId) {
    QueryParameterMap queryParameters;
    ExpressionAlgebrizerContext ctx(true /*assertExprSort*/,
                                    false /*assertPathSort*/,
                                    rootProjection,
                                    prefixId,
                                    queryParameters);
    ABTAggExpressionVisitor visitor(ctx);

    AggExpressionWalker walker(&visitor);
    expression_walker::walk<const Expression>(expr, &walker);
    return ctx.pop();
}

}  // namespace mongo::optimizer
