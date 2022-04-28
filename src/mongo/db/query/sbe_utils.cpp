/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/db/query/sbe_utils.h"

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/logv2/log.h"

namespace mongo::sbe {
namespace {
/**
 * Helper class that collects InputParamIds from IET EvalNodes.
 */
class InputParamIdsCollector {
public:
    InputParamIdsCollector(stdx::unordered_set<MatchExpression::InputParamId>& inputParamIds)
        : _inputParamIds{inputParamIds} {}

    template <typename... Args>
    void transport(Args...) {}

    void transport(const interval_evaluation_tree::EvalNode& node) {
        _inputParamIds.insert(node.inputParamId());
    }

private:
    stdx::unordered_set<MatchExpression::InputParamId>& _inputParamIds;
};

/**
 * Collect InputParamsIds from IETs of the given 'indexBoundsEvaluationInfos' for debugging
 * purposes.
 */
stdx::unordered_set<MatchExpression::InputParamId> collectInputParamIdsFromIETs(
    const std::vector<stage_builder::IndexBoundsEvaluationInfo>& indexBoundsEvaluationInfos) {
    stdx::unordered_set<MatchExpression::InputParamId> inputParamIds;
    InputParamIdsCollector transporter{inputParamIds};
    for (const auto& indexBoundsInfo : indexBoundsEvaluationInfos) {
        for (const auto& iet : indexBoundsInfo.iets) {
            optimizer::algebra::transport(iet, transporter);
        }
    }
    return inputParamIds;
}

struct MatchExpressionInputParamIdsValidatorVisitorContext {
    size_t numberOfIncorrectlyMissingInputParamIds{0};
};

class MatchExpressionInputParamIdsValidatorVisitor final : public MatchExpressionConstVisitor {
public:
    MatchExpressionInputParamIdsValidatorVisitor(
        MatchExpressionInputParamIdsValidatorVisitorContext* context,
        const stdx::unordered_set<MatchExpression::InputParamId>& paramIdsFromIETs,
        const stage_builder::InputParamToSlotMap& inputParamToSlotMap)
        : _context{context},
          _paramIdsFromIETs{paramIdsFromIETs},
          _inputParamToSlotMap{inputParamToSlotMap} {
        invariant(_context);
    }

    void visit(const BitsAllClearMatchExpression* expr) final {
        visitBitTestExpr(expr);
    }

    void visit(const BitsAllSetMatchExpression* expr) final {
        visitBitTestExpr(expr);
    }

    void visit(const BitsAnyClearMatchExpression* expr) final {
        visitBitTestExpr(expr);
    }

    void visit(const BitsAnySetMatchExpression* expr) final {
        visitBitTestExpr(expr);
    }

    void visit(const EqualityMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const GTEMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const GTMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const InMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const LTEMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const LTMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const ModMatchExpression* expr) final {
        validate(expr->getDivisorInputParamId(), expr);
        validate(expr->getRemainderInputParamId(), expr);
    }

    void visit(const RegexMatchExpression* expr) final {
        validate(expr->getCompiledRegexInputParamId(), expr);
        validate(expr->getSourceRegexInputParamId(), expr);
    }

    void visit(const SizeMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const TypeMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const WhereMatchExpression* expr) final {
        visitExpr(expr);
    }

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(const ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    void visitBitTestExpr(const BitTestMatchExpression* expr) {
        validate(expr->getBitPositionsParamId(), expr);
        validate(expr->getBitMaskParamId(), expr);
    }

    template <typename T>
    void visitExpr(const T* expr) {
        validate(expr->getInputParamId(), expr);
    }

    void validate(const boost::optional<MatchExpression::InputParamId>& inputParamId,
                  const MatchExpression* expr) {
        if (inputParamId && !_inputParamToSlotMap.contains(*inputParamId) &&
            !_paramIdsFromIETs.contains(*inputParamId)) {
            ++_context->numberOfIncorrectlyMissingInputParamIds;
            LOGV2_WARNING(6536100,
                          "Found mismatched input param",
                          "inputParamID"_attr = *inputParamId,
                          "expr"_attr = expr->debugString());
        }
    }

    MatchExpressionInputParamIdsValidatorVisitorContext* _context;

    const stdx::unordered_set<MatchExpression::InputParamId>& _paramIdsFromIETs;
    const stage_builder::InputParamToSlotMap& _inputParamToSlotMap;
};
}  // namespace

bool isQuerySbeCompatible(const CollectionPtr* collection,
                          const CanonicalQuery* cq,
                          size_t plannerOptions) {
    tassert(6071400, "Expected CanonicalQuery pointer to not be nullptr", cq);
    invariant(cq);
    auto expCtx = cq->getExpCtxRaw();
    const auto& sortPattern = cq->getSortPattern();
    const bool allExpressionsSupported = expCtx && expCtx->sbeCompatible;
    const bool isNotCount = !(plannerOptions & QueryPlannerParams::IS_COUNT);
    const bool isNotOplog = !cq->nss().isOplog();
    const bool doesNotContainMetadataRequirements = cq->metadataDeps().none();
    const bool doesNotSortOnMetaOrPathWithNumericComponents =
        !sortPattern || std::all_of(sortPattern->begin(), sortPattern->end(), [](auto&& part) {
            return part.fieldPath &&
                !FieldRef(part.fieldPath->fullPath()).hasNumericPathComponents();
        });

    // Queries against a time-series collection are not currently supported by SBE.
    const bool isQueryNotAgainstTimeseriesCollection = !(cq->nss().isTimeseriesBucketsCollection());

    // Queries against a clustered collection are not currently supported by SBE.
    tassert(6038600, "Expected CollectionPtr to not be nullptr", collection);
    const bool isQueryNotAgainstClusteredCollection =
        !(collection->get() && collection->get()->isClustered());

    return allExpressionsSupported && isNotCount && doesNotContainMetadataRequirements &&
        isQueryNotAgainstTimeseriesCollection && isQueryNotAgainstClusteredCollection &&
        doesNotSortOnMetaOrPathWithNumericComponents && isNotOplog;
}

bool validateInputParamsBindings(
    const MatchExpression* root,
    const std::vector<stage_builder::IndexBoundsEvaluationInfo>& indexBoundsEvaluationInfos,
    const stage_builder::InputParamToSlotMap& inputParamToSlotMap) {
    auto inputParamIdsFromIETs = collectInputParamIdsFromIETs(indexBoundsEvaluationInfos);
    MatchExpressionInputParamIdsValidatorVisitorContext context{};
    MatchExpressionInputParamIdsValidatorVisitor visitor{
        &context, inputParamIdsFromIETs, inputParamToSlotMap};
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(root, &walker);
    if (context.numberOfIncorrectlyMissingInputParamIds != 0) {
        LOGV2_WARNING(6536101, "Found mismatched input params", "expr"_attr = root->debugString());
    }
    return context.numberOfIncorrectlyMissingInputParamIds == 0;
}
}  // namespace mongo::sbe
