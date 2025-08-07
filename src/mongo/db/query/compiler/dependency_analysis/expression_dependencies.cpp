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

#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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

    void visit(const ExpressionAbs*) override {}
    void visit(const ExpressionAdd*) override {}
    void visit(const ExpressionAllElementsTrue*) override {}
    void visit(const ExpressionAnd*) override {}
    void visit(const ExpressionAnyElementTrue*) override {}
    void visit(const ExpressionArray*) override {}
    void visit(const ExpressionArrayElemAt*) override {}
    void visit(const ExpressionBitAnd*) override {}
    void visit(const ExpressionBitOr*) override {}
    void visit(const ExpressionBitXor*) override {}
    void visit(const ExpressionBitNot*) override {}
    void visit(const ExpressionFirst*) override {}
    void visit(const ExpressionLast*) override {}
    void visit(const ExpressionObjectToArray*) override {}
    void visit(const ExpressionArrayToObject*) override {}
    void visit(const ExpressionBsonSize*) override {}
    void visit(const ExpressionCeil*) override {}
    void visit(const ExpressionCompare*) override {}
    void visit(const ExpressionConcat*) override {}
    void visit(const ExpressionConcatArrays*) override {}
    void visit(const ExpressionDateAdd*) override {}
    void visit(const ExpressionDateDiff*) override {}
    void visit(const ExpressionDateFromString*) override {}
    void visit(const ExpressionDateFromParts*) override {}
    void visit(const ExpressionDateSubtract*) override {}
    void visit(const ExpressionDateToParts*) override {}
    void visit(const ExpressionDateToString*) override {}
    void visit(const ExpressionDateTrunc*) override {}
    void visit(const ExpressionDivide*) override {}
    void visit(const ExpressionExp*) override {}
    void visit(const ExpressionFilter*) override {}
    void visit(const ExpressionFloor*) override {}
    void visit(const ExpressionFunction*) override {}
    void visit(const ExpressionIn*) override {}
    void visit(const ExpressionIndexOfArray*) override {}
    void visit(const ExpressionIndexOfBytes*) override {}
    void visit(const ExpressionIndexOfCP*) override {}
    void visit(const ExpressionInternalJsEmit*) override {}
    void visit(const ExpressionIsNumber*) override {}
    void visit(const ExpressionLn*) override {}
    void visit(const ExpressionLog*) override {}
    void visit(const ExpressionLog10*) override {}
    void visit(const ExpressionMap*) override {}
    void visit(const ExpressionMod*) override {}
    void visit(const ExpressionMultiply*) override {}
    void visit(const ExpressionNot*) override {}
    void visit(const ExpressionOr*) override {}
    void visit(const ExpressionPow*) override {}
    void visit(const ExpressionRange*) override {}
    void visit(const ExpressionReduce*) override {}
    void visit(const ExpressionReplaceOne*) override {}
    void visit(const ExpressionReplaceAll*) override {}
    void visit(const ExpressionSetDifference*) override {}
    void visit(const ExpressionSetEquals*) override {}
    void visit(const ExpressionSetIntersection*) override {}
    void visit(const ExpressionSetIsSubset*) override {}
    void visit(const ExpressionSetUnion*) override {}
    void visit(const ExpressionSimilarityDotProduct*) override {}
    void visit(const ExpressionSimilarityCosine*) override {}
    void visit(const ExpressionSimilarityEuclidean*) override {}
    void visit(const ExpressionSize*) override {}
    void visit(const ExpressionReverseArray*) override {}
    void visit(const ExpressionSortArray*) override {}
    void visit(const ExpressionSlice*) override {}
    void visit(const ExpressionIsArray*) override {}
    void visit(const ExpressionRound*) override {}
    void visit(const ExpressionSplit*) override {}
    void visit(const ExpressionSqrt*) override {}
    void visit(const ExpressionStrcasecmp*) override {}
    void visit(const ExpressionSubstrBytes*) override {}
    void visit(const ExpressionSubstrCP*) override {}
    void visit(const ExpressionStrLenBytes*) override {}
    void visit(const ExpressionBinarySize*) override {}
    void visit(const ExpressionStrLenCP*) override {}
    void visit(const ExpressionSubtract*) override {}
    void visit(const ExpressionTestApiVersion*) override {}
    void visit(const ExpressionToLower*) override {}
    void visit(const ExpressionToUpper*) override {}
    void visit(const ExpressionTrim*) override {}
    void visit(const ExpressionTrunc*) override {}
    void visit(const ExpressionType*) override {}
    void visit(const ExpressionSubtype*) override {}
    void visit(const ExpressionZip*) override {}
    void visit(const ExpressionConvert*) override {}
    void visit(const ExpressionRegexFind*) override {}
    void visit(const ExpressionRegexFindAll*) override {}
    void visit(const ExpressionRegexMatch*) override {}
    void visit(const ExpressionCurrentDate*) override {}
    void visit(const ExpressionCosine*) override {}
    void visit(const ExpressionSine*) override {}
    void visit(const ExpressionTangent*) override {}
    void visit(const ExpressionArcCosine*) override {}
    void visit(const ExpressionArcSine*) override {}
    void visit(const ExpressionArcTangent*) override {}
    void visit(const ExpressionArcTangent2*) override {}
    void visit(const ExpressionHyperbolicArcTangent*) override {}
    void visit(const ExpressionHyperbolicArcCosine*) override {}
    void visit(const ExpressionHyperbolicArcSine*) override {}
    void visit(const ExpressionHyperbolicTangent*) override {}
    void visit(const ExpressionHyperbolicCosine*) override {}
    void visit(const ExpressionHyperbolicSine*) override {}
    void visit(const ExpressionDegreesToRadians*) override {}
    void visit(const ExpressionRadiansToDegrees*) override {}
    void visit(const ExpressionDayOfMonth*) override {}
    void visit(const ExpressionDayOfWeek*) override {}
    void visit(const ExpressionDayOfYear*) override {}
    void visit(const ExpressionHour*) override {}
    void visit(const ExpressionMillisecond*) override {}
    void visit(const ExpressionMinute*) override {}
    void visit(const ExpressionMonth*) override {}
    void visit(const ExpressionSecond*) override {}
    void visit(const ExpressionWeek*) override {}
    void visit(const ExpressionIsoWeekYear*) override {}
    void visit(const ExpressionIsoDayOfWeek*) override {}
    void visit(const ExpressionIsoWeek*) override {}
    void visit(const ExpressionYear*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>*) override {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>*) override {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>*) override {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>*) override {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>*) override {}
    void visit(const ExpressionTests::Testable*) override {}
    void visit(const ExpressionToHashedIndexKey*) override {}
    void visit(const ExpressionGetField*) override {}
    void visit(const ExpressionSetField*) override {}
    void visit(const ExpressionTsSecond*) override {}
    void visit(const ExpressionTsIncrement*) override {}
    void visit(const ExpressionCond*) override {}
    void visit(const ExpressionSwitch*) override {}
    void visit(const ExpressionConstant*) override {}
    void visit(const ExpressionIfNull*) override {}
    void visit(const ExpressionObject*) override {}
    void visit(const ExpressionInternalFLEBetween*) override {}
    void visit(const ExpressionInternalFLEEqual*) override {}
    void visit(const ExpressionEncStrStartsWith*) override {}
    void visit(const ExpressionEncStrEndsWith*) override {}
    void visit(const ExpressionEncStrContains*) override {}
    void visit(const ExpressionEncStrNormalizedEq*) override {}
    void visit(const ExpressionInternalRawSortKey*) override {}
    void visit(const ExpressionInternalOwningShard*) override {}
    void visit(const ExpressionInternalIndexKey*) override {}
    void visit(const ExpressionInternalKeyStringValue*) override {}
    void visit(const ExpressionTestFeatureFlagLatest*) override {}
    void visit(const ExpressionTestFeatureFlagLastLTS*) override {}
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
        _deps->setNeedsMetadata(expr->getMetaType());
    }

    void visit(const ExpressionInternalRawSortKey* expr) final {
        _deps->setNeedsMetadata(DocumentMetadataFields::MetaType::kSortKey);
    }

    void visit(const ExpressionRandom* expr) final {
        _deps->needRandomGenerator = true;
    }

    void visit(const ExpressionCreateUUID* expr) final {
        _deps->needRandomGenerator = true;
    }

    void visit(const ExpressionCreateObjectId* expr) final {
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
        dependency_analysis::addVariableRefs(&(*expr->getMatchExpr()), _refs);
    }

    void visit(const ExpressionInternalFindElemMatch* expr) final {
        dependency_analysis::addVariableRefs(&(*expr->getMatchExpr()), _refs);
    }

    //
    // These overloads are not defined in the default visitor.
    //
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionLet* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionCreateUUID* expr) final {}
    void visit(const ExpressionCreateObjectId* expr) final {}
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
