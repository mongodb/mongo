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

#include <stack>

#include "mongo/base/error_codes.h"
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/expr_algebrizer_context.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

class ABTAggExpressionVisitor final : public ExpressionConstVisitor {
public:
    ABTAggExpressionVisitor(ExpressionAlgebrizerContext& ctx) : _ctx(ctx){};

    void visit(const ExpressionConstant* expr) override final {
        auto [tag, val] = convertFrom(expr->getValue());
        _ctx.push<Constant>(tag, val);
    }

    void visit(const ExpressionAbs* expr) override final {
        pushSingleArgFunctionFromTop("abs");
    }

    void visit(const ExpressionAdd* expr) override final {
        pushArithmeticBinaryExpr(expr, Operations::Add);
    }

    void visit(const ExpressionAllElementsTrue* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionAnd* expr) override final {
        visitMultiBranchLogicExpression(expr, Operations::And);
    }

    void visit(const ExpressionAnyElementTrue* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionArray* expr) override final {
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

    void visit(const ExpressionArrayElemAt* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFirst* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLast* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionObjectToArray* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionArrayToObject* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionBsonSize* expr) override final {
        pushSingleArgFunctionFromTop("bsonSize");
    }

    void visit(const ExpressionCeil* expr) override final {
        pushSingleArgFunctionFromTop("ceil");
    }

    void visit(const ExpressionCoerceToBool* expr) override final {
        // Since $coerceToBool is internal-only and there are not yet any input expressions that
        // generate an ExpressionCoerceToBool expression, we will leave it as unreachable for now.
        MONGO_UNREACHABLE;
    }

    void visit(const ExpressionCompare* expr) override final {
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
            PathAppender appender(make<PathCompare>(op, std::move(expr)));
            appender.append(path);
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
                    std::move(rightPtr->getPath()), std::move(left), reverseComparisonOp(op));
                return;
            }
        }
        _ctx.push<BinaryOp>(op, std::move(left), std::move(right));
    }

    void visit(const ExpressionConcat* expr) override final {
        pushMultiArgFunctionFromTop("concat", expr->getChildren().size());
    }

    void visit(const ExpressionConcatArrays* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionCond* expr) override final {
        _ctx.ensureArity(3);
        ABT cond = _ctx.pop();
        ABT thenCase = _ctx.pop();
        ABT elseCase = _ctx.pop();
        _ctx.push<If>(std::move(cond), std::move(thenCase), std::move(elseCase));
    }

    void visit(const ExpressionDateFromString* expr) override final {
        unsupportedExpression("$dateFromString");
    }

    void visit(const ExpressionDateFromParts* expr) override final {
        unsupportedExpression("$dateFromParts");
    }

    void visit(const ExpressionDateDiff* expr) override final {
        unsupportedExpression("$dateDiff");
    }

    void visit(const ExpressionDateToParts* expr) override final {
        unsupportedExpression("$dateToParts");
    }

    void visit(const ExpressionDateToString* expr) override final {
        unsupportedExpression("$dateToString");
    }

    void visit(const ExpressionDateTrunc* expr) override final {
        unsupportedExpression("$dateTrunc");
    }

    void visit(const ExpressionDivide* expr) override final {
        pushArithmeticBinaryExpr(expr, Operations::Div);
    }

    void visit(const ExpressionExp* expr) override final {
        pushSingleArgFunctionFromTop("exp");
    }

    void visit(const ExpressionFieldPath* expr) override final {
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
            [](const std::string& fieldName, const bool isLastElement, ABT input) {
                if (!isLastElement) {
                    input = make<PathTraverse>(std::move(input), PathTraverse::kUnlimited);
                }
                return make<PathGet>(fieldName, std::move(input));
            },
            1ul);

        _ctx.push<EvalPath>(std::move(path), make<Variable>(_ctx.getRootProjection()));
    }

    void visit(const ExpressionFilter* expr) override final {
        const auto& varId = expr->getVariableId();
        uassert(6624427,
                "Filter variable must be user-defined.",
                Variables::isUserDefinedVariable(varId));
        const std::string& varName = generateVariableName(varId);

        _ctx.ensureArity(2);
        ABT filter = _ctx.pop();
        ABT input = _ctx.pop();

        _ctx.push<EvalPath>(
            make<PathTraverse>(
                make<PathLambda>(make<LambdaAbstraction>(
                    varName,
                    make<If>(std::move(filter), make<Variable>(varName), Constant::nothing()))),
                PathTraverse::kUnlimited),
            std::move(input));
    }

    void visit(const ExpressionFloor* expr) override final {
        pushSingleArgFunctionFromTop("floor");
    }

    void visit(const ExpressionIfNull* expr) override final {
        pushMultiArgFunctionFromTop("ifNull", 2);
    }

    void visit(const ExpressionIn* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfArray* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfBytes* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfCP* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIsNumber* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLet* expr) override final {
        unsupportedExpression("$let");
    }

    void visit(const ExpressionLn* expr) override final {
        pushSingleArgFunctionFromTop("ln");
    }

    void visit(const ExpressionLog* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionLog10* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionInternalFLEEqual* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionMap* expr) override final {
        unsupportedExpression("$map");
    }

    void visit(const ExpressionMeta* expr) override final {
        unsupportedExpression("$meta");
    }

    void visit(const ExpressionMod* expr) override final {
        pushMultiArgFunctionFromTop("mod", 2);
    }

    void visit(const ExpressionMultiply* expr) override final {
        pushArithmeticBinaryExpr(expr, Operations::Mult);
    }

    void visit(const ExpressionNot* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionObject* expr) override final {
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

    void visit(const ExpressionOr* expr) override final {
        visitMultiBranchLogicExpression(expr, Operations::Or);
    }

    void visit(const ExpressionPow* expr) override final {
        unsupportedExpression("$pow");
    }

    void visit(const ExpressionRange* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionReduce* expr) override final {
        unsupportedExpression("$reduce");
    }

    void visit(const ExpressionReplaceOne* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionReplaceAll* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetDifference* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetEquals* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetIntersection* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetIsSubset* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSetUnion* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSize* expr) override final {
        pushSingleArgFunctionFromTop("getArraySize");
    }

    void visit(const ExpressionReverseArray* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSortArray* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSlice* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIsArray* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionRound* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSplit* expr) override final {
        pushMultiArgFunctionFromTop("split", expr->getChildren().size());
    }

    void visit(const ExpressionSqrt* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrcasecmp* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubstrBytes* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubstrCP* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrLenBytes* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionBinarySize* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionStrLenCP* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionSubtract* expr) override final {
        pushArithmeticBinaryExpr(expr, Operations::Sub);
    }

    void visit(const ExpressionSwitch* expr) override final {
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

    void visit(const ExpressionTestApiVersion* expr) override final {
        unsupportedExpression("$_testApiVersion");
    }

    void visit(const ExpressionToLower* expr) override final {
        pushSingleArgFunctionFromTop("toLower");
    }

    void visit(const ExpressionToUpper* expr) override final {
        pushSingleArgFunctionFromTop("toUpper");
    }

    void visit(const ExpressionTrim* expr) override final {
        unsupportedExpression("$trim");
    }

    void visit(const ExpressionTrunc* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionType* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionZip* expr) override final {
        unsupportedExpression("$zip");
    }

    void visit(const ExpressionConvert* expr) override final {
        unsupportedExpression("$convert");
    }

    void visit(const ExpressionRegexFind* expr) override final {
        unsupportedExpression("$regexFind");
    }

    void visit(const ExpressionRegexFindAll* expr) override final {
        unsupportedExpression("$regexFindAll");
    }

    void visit(const ExpressionRegexMatch* expr) override final {
        unsupportedExpression("$regexMatch");
    }

    void visit(const ExpressionCosine* expr) override final {
        pushSingleArgFunctionFromTop("cosine");
    }

    void visit(const ExpressionSine* expr) override final {
        pushSingleArgFunctionFromTop("sine");
    }

    void visit(const ExpressionTangent* expr) override final {
        pushSingleArgFunctionFromTop("tangent");
    }

    void visit(const ExpressionArcCosine* expr) override final {
        pushSingleArgFunctionFromTop("arcCosine");
    }

    void visit(const ExpressionArcSine* expr) override final {
        pushSingleArgFunctionFromTop("arcSine");
    }

    void visit(const ExpressionArcTangent* expr) override final {
        pushSingleArgFunctionFromTop("arcTangent");
    }

    void visit(const ExpressionArcTangent2* expr) override final {
        pushSingleArgFunctionFromTop("arcTangent2");
    }

    void visit(const ExpressionHyperbolicArcTangent* expr) override final {
        pushSingleArgFunctionFromTop("arcTangentH");
    }

    void visit(const ExpressionHyperbolicArcCosine* expr) override final {
        pushSingleArgFunctionFromTop("arcCosineH");
    }

    void visit(const ExpressionHyperbolicArcSine* expr) override final {
        pushSingleArgFunctionFromTop("arcSineH");
    }

    void visit(const ExpressionHyperbolicTangent* expr) override final {
        pushSingleArgFunctionFromTop("tangentH");
    }

    void visit(const ExpressionHyperbolicCosine* expr) override final {
        pushSingleArgFunctionFromTop("cosineH");
    }

    void visit(const ExpressionHyperbolicSine* expr) override final {
        pushSingleArgFunctionFromTop("sineH");
    }

    void visit(const ExpressionDegreesToRadians* expr) override final {
        pushSingleArgFunctionFromTop("degreesToRadians");
    }

    void visit(const ExpressionRadiansToDegrees* expr) override final {
        pushSingleArgFunctionFromTop("radiansToDegrees");
    }

    void visit(const ExpressionDayOfMonth* expr) override final {
        unsupportedExpression("$dayOfMonth");
    }

    void visit(const ExpressionDayOfWeek* expr) override final {
        unsupportedExpression("$dayOfWeek");
    }

    void visit(const ExpressionDayOfYear* expr) override final {
        unsupportedExpression("$dayOfYear");
    }

    void visit(const ExpressionHour* expr) override final {
        unsupportedExpression("$hour");
    }

    void visit(const ExpressionMillisecond* expr) override final {
        unsupportedExpression("$millisecond");
    }

    void visit(const ExpressionMinute* expr) override final {
        unsupportedExpression("$minute");
    }

    void visit(const ExpressionMonth* expr) override final {
        unsupportedExpression("$month");
    }

    void visit(const ExpressionSecond* expr) override final {
        unsupportedExpression("$second");
    }

    void visit(const ExpressionWeek* expr) override final {
        unsupportedExpression("$week");
    }

    void visit(const ExpressionIsoWeekYear* expr) override final {
        unsupportedExpression("$isoWeekYear");
    }

    void visit(const ExpressionIsoDayOfWeek* expr) override final {
        unsupportedExpression("$isoDayOfWeek");
    }

    void visit(const ExpressionIsoWeek* expr) override final {
        unsupportedExpression("$isoWeek");
    }

    void visit(const ExpressionYear* expr) override final {
        unsupportedExpression("$year");
    }

    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionTests::Testable* expr) override final {
        unsupportedExpression("$test");
    }

    void visit(const ExpressionInternalJsEmit* expr) override final {
        unsupportedExpression("$internalJsEmit");
    }

    void visit(const ExpressionInternalFindSlice* expr) override final {
        unsupportedExpression("$internalFindSlice");
    }

    void visit(const ExpressionInternalFindPositional* expr) override final {
        unsupportedExpression("$internalFindPositional");
    }

    void visit(const ExpressionInternalFindElemMatch* expr) override final {
        unsupportedExpression("$internalFindElemMatch");
    }

    void visit(const ExpressionFunction* expr) override final {
        unsupportedExpression("$function");
    }

    void visit(const ExpressionRandom* expr) override final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionToHashedIndexKey* expr) override final {
        unsupportedExpression("$toHashedIndexKey");
    }

    void visit(const ExpressionDateAdd* expr) override final {
        unsupportedExpression("dateAdd");
    }

    void visit(const ExpressionDateSubtract* expr) override final {
        unsupportedExpression("dateSubtract");
    }

    void visit(const ExpressionSetField* expr) override final {
        unsupportedExpression("$setField");
    }

    void visit(const ExpressionGetField* expr) override final {
        unsupportedExpression("$getField");
    }

    void visit(const ExpressionTsSecond* expr) override final {
        unsupportedExpression("tsSecond");
    }

    void visit(const ExpressionTsIncrement* expr) override final {
        unsupportedExpression("tsIncrement");
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

    std::string generateVariableName(const Variables::Id varId) {
        std::ostringstream os;
        os << _ctx.getUniqueIdPrefix() << "_var_" << varId;
        return os.str();
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
                          const std::string& rootProjection,
                          const std::string& uniqueIdPrefix) {
    ExpressionAlgebrizerContext ctx(
        true /*assertExprSort*/, false /*assertPathSort*/, rootProjection, uniqueIdPrefix);
    ABTAggExpressionVisitor visitor(ctx);

    AggExpressionWalker walker(&visitor);
    expression_walker::walk<const Expression>(expr, &walker);
    return ctx.pop();
}

}  // namespace mongo::optimizer
