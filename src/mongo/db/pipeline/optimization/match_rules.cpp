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
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_list_sessions.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {
namespace {
/**
 * Verifies whether or not a $group is able to swap with a succeeding $match stage. While ordinarily
 * $group can swap with a $match, it cannot if the following $match has exactly one field as the
 * $group key and either:
 *     (1) an $exists predicate on _id
 *     (2) a $type predicate on _id
 *
 * For $exists, every document will have an _id field following such a $group stage, including those
 * whose group key was missing before the $group. As an example, the following optimization would be
 * incorrect as the post-optimization pipeline would handle documents that had nullish _id fields
 * differently. Thus, given such a $group and $match, this function would return false.
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
bool isChangeStreamRouterPipelineStage(StringData stageName) {
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
        ctx.addRules({kPushMatchBeforeChangeStreams});
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

template <bool shouldAdvance>
bool pushdownMatch(PipelineRewriteContext& ctx, DocumentSource& prev, DocumentSourceMatch& match) {
    auto [renameableMatchPart, nonRenameableMatchPart] =
        DocumentSourceMatch::splitMatchByModifiedFields(&match, prev.getModifiedPaths());
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

REGISTER_RULES(DocumentSourceInternalChangeStreamMatch,
               OPTIMIZE_AT_RULE(DocumentSourceInternalChangeStreamMatch),
               OPTIMIZE_IN_PLACE_RULE(DocumentSourceInternalChangeStreamMatch),
               {
                   .name = "INTERNAL_CHANGE_STREAM_MATCH_PUSHDOWN",
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
}  // namespace mongo::rule_based_rewrites::pipeline
