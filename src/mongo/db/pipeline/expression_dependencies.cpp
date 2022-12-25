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

#include "mongo/db/pipeline/expression_dependencies.h"

#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"

namespace mongo::expression {

namespace {

/**
 * The overwhelming majority of expressions do not need to participate in dependency analysis, so
 * this class avoids duplicating the boilerplate. However, when adding to this list, consider
 * whether the new expression can refer to a field path or a variable and add it to the specific
 * visitor below.
 */
class DefaultDependencyVisitor : public ExpressionConstVisitor {
public:
    // To avoid overloaded-virtual warnings.
    using ExpressionConstVisitor::visit;

    void visit(const ExpressionAbs*) {}
    void visit(const ExpressionAdd*) {}
    void visit(const ExpressionAllElementsTrue*) {}
    void visit(const ExpressionAnd*) {}
    void visit(const ExpressionAnyElementTrue*) {}
    void visit(const ExpressionArray*) {}
    void visit(const ExpressionArrayElemAt*) {}
    void visit(const ExpressionBitAnd*) {}
    void visit(const ExpressionBitOr*) {}
    void visit(const ExpressionBitXor*) {}
    void visit(const ExpressionBitNot*) {}
    void visit(const ExpressionFirst*) {}
    void visit(const ExpressionLast*) {}
    void visit(const ExpressionObjectToArray*) {}
    void visit(const ExpressionArrayToObject*) {}
    void visit(const ExpressionBsonSize*) {}
    void visit(const ExpressionCeil*) {}
    void visit(const ExpressionCoerceToBool*) {}
    void visit(const ExpressionCompare*) {}
    void visit(const ExpressionConcat*) {}
    void visit(const ExpressionConcatArrays*) {}
    void visit(const ExpressionDateAdd*) {}
    void visit(const ExpressionDateDiff*) {}
    void visit(const ExpressionDateFromString*) {}
    void visit(const ExpressionDateFromParts*) {}
    void visit(const ExpressionDateSubtract*) {}
    void visit(const ExpressionDateToParts*) {}
    void visit(const ExpressionDateToString*) {}
    void visit(const ExpressionDateTrunc*) {}
    void visit(const ExpressionDivide*) {}
    void visit(const ExpressionExp*) {}
    void visit(const ExpressionFilter*) {}
    void visit(const ExpressionFloor*) {}
    void visit(const ExpressionFunction*) {}
    void visit(const ExpressionIn*) {}
    void visit(const ExpressionIndexOfArray*) {}
    void visit(const ExpressionIndexOfBytes*) {}
    void visit(const ExpressionIndexOfCP*) {}
    void visit(const ExpressionInternalJsEmit*) {}
    void visit(const ExpressionIsNumber*) {}
    void visit(const ExpressionLn*) {}
    void visit(const ExpressionLog*) {}
    void visit(const ExpressionLog10*) {}
    void visit(const ExpressionMap*) {}
    void visit(const ExpressionMod*) {}
    void visit(const ExpressionMultiply*) {}
    void visit(const ExpressionNot*) {}
    void visit(const ExpressionOr*) {}
    void visit(const ExpressionPow*) {}
    void visit(const ExpressionRange*) {}
    void visit(const ExpressionReduce*) {}
    void visit(const ExpressionReplaceOne*) {}
    void visit(const ExpressionReplaceAll*) {}
    void visit(const ExpressionSetDifference*) {}
    void visit(const ExpressionSetEquals*) {}
    void visit(const ExpressionSetIntersection*) {}
    void visit(const ExpressionSetIsSubset*) {}
    void visit(const ExpressionSetUnion*) {}
    void visit(const ExpressionSize*) {}
    void visit(const ExpressionReverseArray*) {}
    void visit(const ExpressionSortArray*) {}
    void visit(const ExpressionSlice*) {}
    void visit(const ExpressionIsArray*) {}
    void visit(const ExpressionRound*) {}
    void visit(const ExpressionSplit*) {}
    void visit(const ExpressionSqrt*) {}
    void visit(const ExpressionStrcasecmp*) {}
    void visit(const ExpressionSubstrBytes*) {}
    void visit(const ExpressionSubstrCP*) {}
    void visit(const ExpressionStrLenBytes*) {}
    void visit(const ExpressionBinarySize*) {}
    void visit(const ExpressionStrLenCP*) {}
    void visit(const ExpressionSubtract*) {}
    void visit(const ExpressionTestApiVersion*) {}
    void visit(const ExpressionToLower*) {}
    void visit(const ExpressionToUpper*) {}
    void visit(const ExpressionTrim*) {}
    void visit(const ExpressionTrunc*) {}
    void visit(const ExpressionType*) {}
    void visit(const ExpressionZip*) {}
    void visit(const ExpressionConvert*) {}
    void visit(const ExpressionRegexFind*) {}
    void visit(const ExpressionRegexFindAll*) {}
    void visit(const ExpressionRegexMatch*) {}
    void visit(const ExpressionCosine*) {}
    void visit(const ExpressionSine*) {}
    void visit(const ExpressionTangent*) {}
    void visit(const ExpressionArcCosine*) {}
    void visit(const ExpressionArcSine*) {}
    void visit(const ExpressionArcTangent*) {}
    void visit(const ExpressionArcTangent2*) {}
    void visit(const ExpressionHyperbolicArcTangent*) {}
    void visit(const ExpressionHyperbolicArcCosine*) {}
    void visit(const ExpressionHyperbolicArcSine*) {}
    void visit(const ExpressionHyperbolicTangent*) {}
    void visit(const ExpressionHyperbolicCosine*) {}
    void visit(const ExpressionHyperbolicSine*) {}
    void visit(const ExpressionDegreesToRadians*) {}
    void visit(const ExpressionRadiansToDegrees*) {}
    void visit(const ExpressionDayOfMonth*) {}
    void visit(const ExpressionDayOfWeek*) {}
    void visit(const ExpressionDayOfYear*) {}
    void visit(const ExpressionHour*) {}
    void visit(const ExpressionMillisecond*) {}
    void visit(const ExpressionMinute*) {}
    void visit(const ExpressionMonth*) {}
    void visit(const ExpressionSecond*) {}
    void visit(const ExpressionWeek*) {}
    void visit(const ExpressionIsoWeekYear*) {}
    void visit(const ExpressionIsoDayOfWeek*) {}
    void visit(const ExpressionIsoWeek*) {}
    void visit(const ExpressionYear*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>*) {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>*) {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>*) {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>*) {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>*) {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>*) {}
    void visit(const ExpressionTests::Testable*) {}
    void visit(const ExpressionToHashedIndexKey*) {}
    void visit(const ExpressionGetField*) {}
    void visit(const ExpressionSetField*) {}
    void visit(const ExpressionTsSecond*) {}
    void visit(const ExpressionTsIncrement*) {}
    void visit(const ExpressionCond*) {}
    void visit(const ExpressionSwitch*) {}
    void visit(const ExpressionConstant*) {}
    void visit(const ExpressionIfNull*) {}
    void visit(const ExpressionObject*) {}
    void visit(const ExpressionInternalFLEEqual*) {}
    void visit(const ExpressionInternalFLEBetween*) {}
    void visit(const ExpressionInternalOwningShard*) {}
    void visit(const ExpressionInternalIndexKey*) {}
};

class DependencyVisitor : public DefaultDependencyVisitor {
public:
    // To avoid overloaded-virtual warnings.
    using DefaultDependencyVisitor::visit;

    DependencyVisitor(DepsTracker* deps) : _deps(deps) {}

    void visit(const ExpressionLet* expr) final {
        for (auto&& idToNameExp : expr->getVariableMap()) {
            // Add the external dependencies from the 'vars' statement.
            addDependencies(idToNameExp.second.expression.get(), _deps);
        }
    }

    void visit(const ExpressionMeta* expr) final {
        using MetaType = DocumentMetadataFields::MetaType;

        auto metaType = expr->getMetaType();
        if (metaType == MetaType::kSearchScore || metaType == MetaType::kSearchHighlights ||
            metaType == MetaType::kSearchScoreDetails) {
            // We do not add the dependencies for searchScore, searchHighlights, or
            // searchScoreDetails because those values are not stored in the collection (or in
            // mongod at all).
            return;
        }

        _deps->setNeedsMetadata(metaType, true);
    }

    void visit(const ExpressionRandom* expr) final {
        _deps->needRandomGenerator = true;
    }

    void visit(const ExpressionFieldPath* expr) final {
        if (!expr->isVariableReference()) {  // includes CURRENT when it is equivalent to ROOT.
            if (expr->getFieldPath().getPathLength() == 1) {
                _deps->needWholeDocument = true;  // need full doc if just "$$ROOT"
            } else {
                _deps->fields.insert(expr->getFieldPath().tail().fullPath());
            }
        }
    }

    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        auto fp = expr->getFieldPath();
        // We require everything below the first field.
        _deps->fields.insert(std::string(fp.getSubpath(0)));
    }

    void visit(const ExpressionInternalFindPositional* expr) final {
        _deps->needWholeDocument = true;
    }

    void visit(const ExpressionInternalFindSlice* expr) final {
        _deps->needWholeDocument = true;
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        _deps->needWholeDocument = true;
    }

private:
    DepsTracker* _deps;
};

class VariableRefVisitor : public DefaultDependencyVisitor {
public:
    // To avoid overloaded-virtual warnings.
    using DefaultDependencyVisitor::visit;

    VariableRefVisitor(std::set<Variables::Id>* refs) : _refs(refs) {}

    void visit(const ExpressionFieldPath* expr) final {
        auto varId = expr->getVariableId();
        if (varId != Variables::kRootId) {
            _refs->insert(varId);
        }
    }

    void visit(const ExpressionInternalFindPositional* expr) final {
        match_expression::addVariableRefs(&(*expr->getMatchExpr()), _refs);
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        match_expression::addVariableRefs(&(*expr->getMatchExpr()), _refs);
    }

    //
    // These overloads are not defined in the default visitor.
    //
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionLet* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {}

private:
    std::set<Variables::Id>* _refs;
};

template <typename VisitorType>
class ExpressionWalker final {
public:
    ExpressionWalker(VisitorType* visitor) : _visitor{visitor} {}

    void postVisit(const Expression* expr) {
        expr->acceptVisitor(_visitor);
    }

private:
    VisitorType* _visitor;
};

}  // namespace

void addDependencies(const Expression* expr, DepsTracker* deps) {
    DependencyVisitor visitor(deps);
    ExpressionWalker walker(&visitor);
    expression_walker::walk<const Expression>(expr, &walker);
}

DepsTracker getDependencies(const Expression* expr) {
    DepsTracker deps;
    addDependencies(expr, &deps);
    return deps;
}

void addVariableRefs(const Expression* expr, std::set<Variables::Id>* refs) {
    VariableRefVisitor visitor(refs);
    ExpressionWalker walker(&visitor);
    expression_walker::walk<const Expression>(expr, &walker);

    // Filter out references to any local variables.
    if (auto boundVariable = expr->getBoundaryVariableId(); boundVariable) {
        refs->erase(refs->upper_bound(*boundVariable), refs->end());
    }
}

}  // namespace mongo::expression
