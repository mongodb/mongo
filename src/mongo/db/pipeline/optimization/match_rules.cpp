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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/logv2/log.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {
namespace {
/**
 * Verifies that an Expression will not change its result if the containing $match is swapped with
 * $group, accounting for $group semantics.
 *
 * Requires the caller to have established that the expression depends on a $group _id field.
 */
class ExpressionGroupSwapValidator
    : public SelectiveConstExpressionVisitorBase<ExpressionGroupSwapValidator> {
public:
    using SelectiveConstExpressionVisitorBase<ExpressionGroupSwapValidator>::visit;

    void visit(const ExpressionCompare* expr) override {
        // We only allow field paths and constants.
        const auto& operands = expr->getOperandList();
        if (!validateCompareOperands(operands)) {
            isValid = false;
        }
    }

    void visit(const ExpressionIn* expr) override {
        const auto& operands = expr->getOperandList();
        if (!validateInOperands(operands)) {
            isValid = false;
        }
    }

    void visit(const ExpressionAnd* expr) override {
        visitLogicOperands(expr->getOperandList());
    }

    void visit(const ExpressionOr* expr) override {
        visitLogicOperands(expr->getOperandList());
    }

    void visit(const ExpressionNot* expr) override {
        visitLogicOperands(expr->getOperandList());
    }

    template <typename T>
    void visitDefault(const T* expr) {
        // Everything else is disallowed by default.
        isValid = false;
    }

    bool isValid{true};

private:
    static boost::intrusive_ptr<Expression> optimizeClone(
        const boost::intrusive_ptr<Expression>& expr) {
        return expr->clone(*expr->getExpressionContext())->optimize();
    }

    static bool validateCompareOperands(const Expression::ExpressionVector& operands) {
        tassert(11277200, "ExpressionCompare should have two operands", operands.size() == 2);

        // Optimize operands so that constant sub-expressions (e.g. arithmetic on literals)
        // are folded into ExpressionConstant before we inspect them.
        auto firstArg = optimizeClone(operands[0]);
        auto secondArg = optimizeClone(operands[1]);

        const ExpressionConstant* constantExpr =
            dynamic_cast<const ExpressionConstant*>(firstArg.get());
        const Expression* otherExpr = secondArg.get();
        if (!constantExpr) {
            constantExpr = dynamic_cast<const ExpressionConstant*>(secondArg.get());
            otherExpr = firstArg.get();
        }
        if (!constantExpr) {
            return false;
        }

        // Do not allow null since $group buckets null and missing together.
        if (constantExpr->getValue().nullish()) {
            return false;
        }

        // We only allow ExpressionFieldPath for the non-constant operand.
        return dynamic_cast<const ExpressionFieldPath*>(otherExpr) != nullptr;
    }

    static bool validateInOperands(const Expression::ExpressionVector& operands) {
        // $in has exactly two operands: the element to test and the array to test against.
        tassert(11277203, "ExpressionIn should have two operands", operands.size() == 2);

        // The element being tested must be a field path. Nothing optimizes to ExpressionFieldPath,
        // so there is no need to call optimize() here.
        if (!dynamic_cast<const ExpressionFieldPath*>(operands[0].get())) {
            return false;
        }

        // Optimize the array operand so that an ExpressionArray of constant children
        // (e.g. [3, 4]) folds into a single ExpressionConstant.
        auto candidateArray = optimizeClone(operands[1]);

        // The array must have optimized down to a single ExpressionConstant.
        const auto* constArray = dynamic_cast<const ExpressionConstant*>(candidateArray.get());
        if (!constArray || constArray->getValue().getType() != BSONType::array) {
            return false;
        }

        // Do not allow null elements since $group buckets null and missing together.
        const auto constArrayValue = constArray->getValue();
        for (const auto& elem : constArrayValue.getArray()) {
            if (elem.nullish()) {
                return false;
            }
        }
        return true;
    }

    void visitLogicOperands(const Expression::ExpressionVector& operands) {
        for (auto&& op : operands) {
            if (!isValid) {
                return;
            }
            op->acceptVisitor(this);
        }
    }
};

/**
 * Validates the $expr predicates within a MatchExpression can be swapped with $group safely. If
 * there's an incompatible $expr predicate that depends on the _id field we cannot swap: $group
 * places null and missing in the same bucket, and numerics that compare equal (e.g. Int(1) and
 * Long(1)) share a bucket, so the _id value may differ from the source field.
 */
bool validateExprMatchExpressionsForSwapWithGroup(const MatchExpression& expr) {
    struct ExprValidator : SelectiveMatchExpressionVisitorBase<true> {
        using SelectiveMatchExpressionVisitorBase<true>::visit;

        void visit(const ExprMatchExpression* expr) override {
            if (!isValid || expression::isIndependentOfConst(*expr, idFields)) {
                return;
            }
            ExpressionGroupSwapValidator validator;
            expr->getExpression()->acceptVisitor(&validator);
            if (!validator.isValid) {
                isValid = false;
            }
        }

        const OrderedPathSet idFields{"_id"};
        bool isValid{true};
    };

    ExprValidator visitor;
    MatchExpressionWalker walker{&visitor, nullptr, nullptr};
    tree_walker::walk<true, MatchExpression>(&expr, &walker);
    return visitor.isValid;
}

/**
 * Verifies whether or not a $group is able to swap with a succeeding $match stage.
 * The swap is allowable because $group reports the _id fields as renames.
 * Example:
 *   {$group: {_id: "$x"}}
 * Reports that $group simply renames x -> _id.
 *
 * Most $match predicates on the _id field can therefore be pushed down. This function guards
 * against pushdown of $match which contains predicates sensitive to the $group bucketing behaviour.
 * $group places null and missing in the same bucket, and numerics that compare equal (e.g. Int(1)
 * and Long(1)) share a bucket, so the _id value may differ from the source field.
 *
 * This affects $exists checks (since after $group we always have null), $type checks, $expr which
 * distinguishes null/missing and can contain other unsafe expressions.
 *
 * As an example, the following optimization would be incorrect as the post-optimization pipeline
 * would handle documents that had nullish _id fields differently. Thus, given such a $group and
 * $match, this function would return false.
 *   {$group: {_id: "$x"}}
 *   {$match: {_id: {$exists: true}}
 * ---->
 *   {$match: {x: {$exists: true}}
 *   {$group: {_id: "$x"}}
 * However with a compound _id spec (something of the form {_id: {x: ..., y: ..., ...}}) existence
 * predicates would be correct to push before as these preserve missing.
 * Note: singular id specs can also be of the form {_id: {x: ...}}.
 *
 * For $type, the $type operator can distinguish between values that compare equal in the $group
 * stage, meaning documents that are regarded unequally in the $match stage are equated in the
 * $group stage. This leads to varied results depending on the order of the $match and $group.
 * Type predicates are incorrect to push ahead regardless of _id spec.
 *
 * For $expr, we allow only certain $expr shapes. Any $expr that is not explicitly allowed is
 * disallowed (so we don't have to prove that arbitrary complex expressions depending on `_id` are
 * safe to swap before $group).
 */
bool groupMatchSwapVerified(const DocumentSourceMatch& nextMatch,
                            const DocumentSourceGroup& thisGroup) {
    // Construct a set of id fields.
    OrderedPathSet idFields;
    for (const auto& key : thisGroup.getIdFieldNames()) {
        idFields.insert(std::string("_id.").append(key));
    }

    // getIdFieldsNames will be 0 if the spec is of the forms {_id: ...}.
    if (idFields.empty()) {
        idFields.insert("_id");
    }

    // If there's any type predicate we cannot swap.
    if (expression::hasPredicateOnPaths(*(nextMatch.getMatchExpression()),
                                        MatchExpression::MatchType::TYPE_OPERATOR,
                                        idFields)) {
        return false;
    }

    if (!validateExprMatchExpressionsForSwapWithGroup(*nextMatch.getMatchExpression())) {
        return false;
    }

    /**
     * If there's a compound _id spec (e.g. {_id: {x: ..., y: ..., ...}}), we can swap regardless of
     * existence predicate.
     * getIdFields will be 1 if the spec is of the forms {_id: ...} or {_id: {x: ...}}.
     */
    if (thisGroup.getIdFields().size() != 1) {
        return true;
    }

    // If there's an existence predicate in a non-compound _id spec, we cannot swap.
    return !expression::hasPredicateOnPaths(
        *(nextMatch.getMatchExpression()), MatchExpression::MatchType::EXISTS, idFields);
}

/**
 * Returns 'true' if the given stage is an internal change stream stage that can appear in a router
 * (mongoS) pipeline, or 'false' otherwise.
 */
bool isChangeStreamRouterPipelineStage(std::string_view stageName) {
    return change_stream_constants::kChangeStreamRouterPipelineStages.contains(stageName);
}

const PipelineRewriteRule kPushMatchBeforeChangeStreams{
    .name = "PUSH_MATCH_BEFORE_CHANGE_STREAMS",
    .precondition = alwaysTrue,
    .transform = Transforms::swapStageWithPrev,
    .priority = kDefaultPushdownPriority,
    .tags = PipelineRewriteContext::Tags::Reordering,
};

bool canPushMatchBefore(PipelineRewriteContext& ctx,
                        const DocumentSource* prev,
                        const DocumentSourceMatch* match) {
    if (!prev || !match) {
        return false;
    }

    // We do not need to attempt this optimization if the $match contains a text search predicate
    // because, in that scenario, $match is already required to be the first stage in the pipeline.
    if (match->isTextQuery()) {
        return false;
    }

    if (!prev->constraints().canSwapWithMatch) {
        return false;
    }

    // At this point:
    // 1) The 'prev' stage is eligible to swap with a $match.
    // 2) The $match stage does not contain a text search predicate.

    // TODO SERVER-55492: Remove the following workaround when there are rename checks for 'other'
    // match expressions.
    if (isChangeStreamRouterPipelineStage(prev->getSourceName())) {
        // Always move the $match stage ahead of internal change stream stages appearing in the
        // router (mongoS) pipeline, because they do not access or modify any paths in the input
        // document.
        ctx.addRule(kPushMatchBeforeChangeStreams);
        return false;
    }

    auto group = dynamic_cast<const DocumentSourceGroup*>(prev);
    if (group && !groupMatchSwapVerified(*match, *group)) {
        return false;
    }

    return true;
}

bool matchCanSwapWithPrecedingStage(PipelineRewriteContext& ctx) {
    if (ctx.atFirstStage()) {
        // $match already at front of pipeline.
        return false;
    }

    return canPushMatchBefore(ctx, ctx.prevStage().get(), &ctx.currentAs<DocumentSourceMatch>());
}

bool canSwapWithSubsequentMatch(PipelineRewriteContext& ctx) {
    if (ctx.atLastStage()) {
        // Already at the end of the pipeline.
        return false;
    }

    return canPushMatchBefore(
        ctx, &ctx.current(), dynamic_cast<DocumentSourceMatch*>(ctx.nextStage().get()));
}

DocumentSource::GetModPathsReturn buildModPaths(PipelineRewriteContext& ctx, DocumentSource& prev) {
    using namespace document_transformation;

    if (!feature_flags::gFeatureFlagImprovedDepsAnalysis.checkEnabled()) {
        return toGetModPathsReturn(prev);
    }

    auto canPathBeArray = [&](std::string_view path) {
        return ctx.getDependencyGraph().canPathBeArray(&prev, path);
    };

    return toGetModPathsReturn(withArraynessInfo(prev, canPathBeArray));
}

template <bool shouldAdvance>
bool pushdownMatch(PipelineRewriteContext& ctx, DocumentSource& prev, DocumentSourceMatch& match) {
    auto [renameableMatchPart, nonRenameableMatchPart] =
        DocumentSourceMatch::splitMatchByModifiedFields(&match, buildModPaths(ctx, prev));
    tassert(11010400,
            "Both sides can't be null after splitting a $match",
            renameableMatchPart || nonRenameableMatchPart);
    if (!renameableMatchPart) {
        return false;
    }

    LOGV2_DEBUG(11010403,
                5,
                "Swapping all or part of a $match stage in front of another stage: ",
                "matchMovingBefore"_attr = redact(renameableMatchPart->serializeToBSONForDebug()),
                "thisStage"_attr = redact(prev.serializeToBSONForDebug()),
                "matchLeftAfter"_attr = redact(
                    nonRenameableMatchPart ? nonRenameableMatchPart->serializeToBSONForDebug()
                                           : BSONObj()));

    // 'partialPushdown()' assumes that the $match we're pushing down is at the current position. If
    // that is not the case, we need to advance.
    if constexpr (shouldAdvance) {
        ctx.advance();
    }

    return Transforms::partialPushdown(
        ctx, std::move(renameableMatchPart), std::move(nonRenameableMatchPart));
}

/**
 * Tries to push a $match stage at the current position before the preceding stage.
 */
bool pushMatchBeforePrecedingStage(PipelineRewriteContext& ctx) {
    auto& prev = *ctx.prevStage();
    auto& match = ctx.currentAs<DocumentSourceMatch>();
    return pushdownMatch<false>(ctx, prev, match);
}

/**
 * Tries to push a subsequent $match stage before the current position.
 */
bool pushMatchBeforeCurrentStage(PipelineRewriteContext& ctx) {
    auto& prev = ctx.current();
    auto& match = checked_cast<DocumentSourceMatch&>(*ctx.nextStage());
    return pushdownMatch<true>(ctx, prev, match);
}

/**
 * Checks whether a specific match operator is a "testing" match expresions for path arrayness.
 * Specifically,
 * i. predicate on field "test",
 * ii. does not include a $and with $type predicate.
 */
bool matchContainsTestingField(PipelineRewriteContext& ctx) {
    auto& match = checked_cast<DocumentSourceMatch&>(ctx.current());
    auto me = match.getMatchExpression();

    if (me->path().find("test") == std::string_view::npos) {
        return false;
    }

    if (me->matchType() == MatchExpression::MatchType::AND) {
        auto andME = checked_cast<AndMatchExpression*>(me);
        if (andME != nullptr) {
            for (const std::unique_ptr<MatchExpression>& andChildME : andME->getChildren()) {
                if (checked_cast<TypeMatchExpression*>(andChildME.get()) != nullptr) {
                    return false;
                }
            }
        }
    }

    return true;
}

/**
 * For a match operator, replace the original DocumentSourceMatch with a composite $and operator
 * including:
 * i. the original predicate,
 * ii. an additional predicate filtering on the type of predicate field comparing to array type.
 */
bool introduceArrayTypeFilteringToMatch(PipelineRewriteContext& ctx) {
    auto& match = checked_cast<DocumentSourceMatch&>(ctx.current());
    auto bsonObj = match.getMatchExpression()->serialize();

    auto additionalFilter = std::make_unique<AndMatchExpression>();
    {
        StatusWithMatchExpression copyME = MatchExpressionParser::parse(bsonObj, &ctx.getExpCtx());
        std::unique_ptr<MatchExpression> me(std::move(copyME.getValue()));

        auto query = match.getQuery();
        for (const auto& elem : query) {
            auto typeExpr = std::make_unique<TypeMatchExpression>(
                std::string_view(elem.fieldNameStringData()), MatcherTypeSet(BSONType::array));
            if (ctx.getExpCtx().canPathBeArrayForNss(FieldPath(elem.fieldNameStringData()),
                                                     ctx.getExpCtx().getNamespaceString())) {
                additionalFilter->add(std::move(typeExpr));
            } else {
                additionalFilter->add(std::make_unique<NotMatchExpression>(std::move(typeExpr)));
            }
        }
        additionalFilter->add(std::move(me));
    }

    BSONObjBuilder out;
    additionalFilter->serialize(&out);
    Transforms::replaceCurrentStage(ctx, DocumentSourceMatch::create(out.obj(), &ctx.getExpCtx()));

    return false;
}

/**
 * Checks whether the $match uses the array type predicate on a path which is known to not contain
 * arrays.
 */
bool matchOnArrayTypeIsAlwaysFalse(PipelineRewriteContext& ctx) {
    if (!feature_flags::gFeatureFlagImprovedDepsAnalysis.checkEnabled()) {
        return false;
    }

    auto& match = checked_cast<DocumentSourceMatch&>(ctx.current());
    auto& expr = *match.getMatchExpression();

    if (expr.matchType() == MatchExpression::MatchType::TYPE_OPERATOR) {
        auto& typeExpr = checked_cast<TypeMatchExpression&>(expr);
        const auto& typeSet = typeExpr.typeSet();
        if (typeSet.isSingleType() && typeSet.hasType(BSONType::array)) {
            const auto& graph = ctx.getDependencyGraph();
            bool canBeArray = graph.canPathBeArray(&ctx.current(), typeExpr.path());
            if (!canBeArray) {
                return true;
            }
        }
    }

    return false;
}

/**
 * Replace the current stage with a $match which never matches any documents.
 */
bool replaceWithAlwaysFalseMatch(PipelineRewriteContext& ctx) {
    auto eofMatch = DocumentSourceMatch::create(BSON("$alwaysFalse" << 1), &ctx.getExpCtx());
    Transforms::replaceCurrentStage(ctx, eofMatch);
    return true;
}

}  // namespace

REGISTER_RULES(DocumentSourceMatch,
               OPTIMIZE_AT_RULE(DocumentSourceMatch),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceMatch),
               {
                   .name = "MATCH_PUSHDOWN",
                   .precondition = matchCanSwapWithPrecedingStage,
                   .transform = pushMatchBeforePrecedingStage,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               });

REGISTER_RULES(DocumentSourceListSessions,
               OPTIMIZE_AT_RULE(DocumentSourceListSessions),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceListSessions),
               {
                   .name = "LIST_SESSIONS_MATCH_PUSHDOWN",
                   .precondition = matchCanSwapWithPrecedingStage,
                   .transform = pushMatchBeforePrecedingStage,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               });

// Projection rewrites require $match pushdown to be attempted before the other rules for the stage.
REGISTER_RULES_WITH_FEATURE_FLAG(DocumentSourceSingleDocumentTransformation,
                                 &feature_flags::gFeatureFlagImprovedDepsAnalysis,
                                 {
                                     .name = "PUSH_MATCH_BEFORE_SINGLE_DOC_TRANSFORMATION",
                                     .precondition = canSwapWithSubsequentMatch,
                                     .transform = pushMatchBeforeCurrentStage,
                                     .priority = kDefaultPushdownPriority,
                                     .tags = PipelineRewriteContext::Tags::Reordering,
                                 });

// Timeseries rewrites require $match pushdown to be attempted before the other optimizations
// implemented in 'optimizeAt()'.
REGISTER_RULES(DocumentSourceInternalUnpackBucket,
               OPTIMIZE_AT_RULE(DocumentSourceInternalUnpackBucket),
               {
                   .name = "PUSH_MATCH_BEFORE_UNPACK_BUCKET",
                   .precondition = canSwapWithSubsequentMatch,
                   .transform = pushMatchBeforeCurrentStage,
                   .priority = kDefaultPushdownPriority,
                   .tags = PipelineRewriteContext::Tags::Reordering,
               });

// Rewrite $match with $type: "array" predicate to $alwaysFalse when the path cannot be array.
// This is currently a Tags::Testing rule, therefore it is only used when the knob is enabled.
REGISTER_RULES(DocumentSourceMatch,
               {
                   "MATCH_ARRAY_TYPE_PREDICATE_ALWAYS_FALSE",
                   matchOnArrayTypeIsAlwaysFalse,
                   replaceWithAlwaysFalseMatch,
                   kDefaultPushdownPriority,
                   PipelineRewriteContext::Tags::Testing,
               });

// Testing rule hidden behind i. the feature flag and ii. Testing tag failpoint.
// Tests whether Aggregation pipelines are accessing correctly the PathArrayness datastructure.
REGISTER_RULES_WITH_FEATURE_FLAG(DocumentSourceMatch,
                                 &feature_flags::gFeatureFlagEnableTestingAggregateRewriteRules,
                                 {"DUMMY_MATCH_CHECK_ARRAYNESS",
                                  matchContainsTestingField,
                                  introduceArrayTypeFilteringToMatch,
                                  1.0,
                                  PipelineRewriteContext::Tags::Testing});

}  // namespace mongo::rule_based_rewrites::pipeline
