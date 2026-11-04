// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"

#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_end_of_transaction.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"
#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <list>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::change_stream::pipeline_helpers {
namespace {

// The only stages that may be re-applied to synthesized events on the router. They are the
// filtering stages and the single-document transformations that the user pipeline can contain.
// Everything else (in particular shard-only stages such as '$changeStreamSplitLargeEvent', which is
// stateful) must not be re-run on the router, because doing so could reorder or drop events.
const StringDataSet kExpectedUserStages = {
    DocumentSourceMatch::kStageName,
    DocumentSourceRedact::kStageName,
    DocumentSourceProject::kStageName,
    DocumentSourceProject::kAliasNameUnset,
    DocumentSourceAddFields::kStageName,
    DocumentSourceAddFields::kAliasNameSet,
    DocumentSourceReplaceRoot::kStageName,
    DocumentSourceReplaceRoot::kAliasNameReplaceWith,
};

}  // namespace

std::vector<BSONObj> buildPipelineForConfigServerV2(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    OperationContext* opCtx,
    Timestamp atClusterTime,
    const NamespaceString& nss,
    const ChangeStream& changeStream,
    ChangeStreamReaderBuilder* readerBuilder) {
    tassert(10657555,
            "expecting v2 change stream version to be set",
            *expCtx->getChangeStreamSpec()->getVersion() == ChangeStreamReaderVersionEnum::kV2);

    // Prepare control events for config server.
    const BSONObj controlEventsFilter =
        readerBuilder->buildControlEventFilterForConfigServer(opCtx, changeStream);

    const std::set<std::string> controlEventTypes =
        readerBuilder->getControlEventTypesOnConfigServer(opCtx, changeStream);

    const DocumentSourceChangeStreamInjectControlEvents::ActionsMap actions = [&]() {
        DocumentSourceChangeStreamInjectControlEvents::ActionsMap result;
        for (auto&& eventName : controlEventTypes) {
            result.emplace(
                eventName,
                DocumentSourceChangeStreamInjectControlEvents::Action::kTransformToControlEvent);
        }
        return result;
    }();

    const BSONObj controlEventsActions =
        DocumentSourceChangeStreamInjectControlEvents::ActionsHelper::serializeToBSON(actions);

    // Set up change stream specification.
    // We need this because we will be passing it to the constructors of the individual change
    // stream pipeline stages.
    DocumentSourceChangeStreamSpec spec;
    spec.setAllowToRunOnConfigDB(true);
    spec.setStartAtOperationTime(atClusterTime);
    spec.setSupportedEvents(
        std::vector<std::string>(controlEventTypes.begin(), controlEventTypes.end()));

    // Create a fresh ExpressionContext for building the pipeline, because some change stream
    // pipeline stages modify the ExpressionContext in their constructors.
    auto clonedExpCtx = makeBlankExpressionContext(opCtx, nss);
    clonedExpCtx->setChangeStreamSpec(spec);

    // No need to update feature counter metrics for the change stream again.
    clonedExpCtx->setUpdateChangeStreamFeatureCounters(false);

    // Build pipeline with change stream stages.
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    // Container to keep memory of backing BSONObjs alive during filter buildup.
    std::vector<BSONObj> backingBsonObjs;

    // Build oplog match stage.
    {
        auto oplogFilter = std::make_unique<AndMatchExpression>();

        // Filter on cluster time.
        oplogFilter->add(change_stream_filter::buildTsFilter(
            clonedExpCtx, atClusterTime, nullptr, backingBsonObjs));

        // Filter out migration events. Migration events can happen on embedded config servers.
        oplogFilter->add(change_stream_filter::buildNotFromMigrateFilter(
            clonedExpCtx, nullptr, backingBsonObjs));

        // Create an $or filter which only captures relevant events in the oplog.
        auto eventFilter = std::make_unique<OrMatchExpression>();
        eventFilter->add(MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(controlEventsFilter), clonedExpCtx));

        eventFilter->add(change_stream_filter::buildTransactionFilterForConfigServer(
            clonedExpCtx, controlEventsFilter, backingBsonObjs));

        // Build the final $match filter to be applied to the oplog.
        oplogFilter->add(std::move(eventFilter));

        stages.push_back(make_intrusive<DocumentSourceChangeStreamOplogMatch>(
            atClusterTime,
            clonedExpCtx,
            optimizeMatchExpression(std::move(oplogFilter),
                                    /* enableSimplification */ false),
            backingBsonObjs));
    }

    // Build unwind transaction stage.
    {
        // The following filter expression will be applied to each unwound transaction entry.
        std::vector<BSONObj> backingBsonObjs;
        auto unwindFilter = std::make_unique<AndMatchExpression>();

        unwindFilter->add(change_stream_filter::buildNotFromMigrateFilter(
            clonedExpCtx, nullptr, backingBsonObjs));

        unwindFilter->add(MatchExpressionParser::parseAndNormalize(
            backingBsonObjs.emplace_back(controlEventsFilter), clonedExpCtx));

        BSONObjBuilder bob;
        unwindFilter->serialize(&bob);

        stages.push_back(
            make_intrusive<DocumentSourceChangeStreamUnwindTransaction>(bob.obj(), clonedExpCtx));
    }

    // Build event transform stage.
    stages.push_back(DocumentSourceChangeStreamTransform::create(clonedExpCtx, spec));

    // Build check resumability stage.
    stages.push_back(DocumentSourceChangeStreamCheckResumability::create(clonedExpCtx, spec));

    // Build inject control events stage.
    stages.push_back(
        DocumentSourceChangeStreamInjectControlEvents::create(clonedExpCtx, controlEventsActions));

    auto pipeline = Pipeline::create(std::move(stages), clonedExpCtx);
    return pipeline->serializeToBson();
}

std::list<boost::intrusive_ptr<DocumentSource>> buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec,
    const ResumeTokenData& resumeToken) {
    const bool useV2ChangeStreamReader = spec.getVersion() == ChangeStreamReaderVersionEnum::kV2;

    // Build the actual change stream pipeline.
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    // Unfold the $changeStream into its constituent stages and add them to the pipeline.
    stages.push_back(DocumentSourceChangeStreamOplogMatch::create(expCtx, spec));
    stages.push_back(DocumentSourceChangeStreamUnwindTransaction::create(expCtx));
    stages.push_back(DocumentSourceChangeStreamTransform::create(expCtx, spec));
    tassert(5666900,
            "'DocumentSourceChangeStreamTransform' stage should populate "
            "'initialPostBatchResumeToken' field",
            !expCtx->getInitialPostBatchResumeToken().isEmpty());

    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    if (DocumentSourceChangeStreamCheckInvalidate::canInvalidateEventOccur(expCtx)) {
        // Invalidate events can only occur in collection-level and database-level change streams,
        // but not in all-cluster change streams.
        stages.push_back(DocumentSourceChangeStreamCheckInvalidate::create(expCtx, spec));
    }

    // Always include a DSCSCheckResumability stage, both to verify that there is enough history to
    // cover the change stream's starting point, and to swallow all events up to the resume point.
    stages.push_back(DocumentSourceChangeStreamCheckResumability::create(expCtx, spec));

    if (expCtx->getInRouter() && useV2ChangeStreamReader) {
        // For V2 change stream readers in sharded clusters, add the DSCSInjectControlEvents stage
        // on the shards. The control events filter will always be for a data-shard when we are
        // here, as the control events stage for a config server is built elsewhere.
        stages.push_back(
            DocumentSourceChangeStreamInjectControlEvents::createForDataShard(expCtx, spec));
    }

    // If 'fullDocumentBeforeChange' is not set to 'off', add the DSCSAddPreImage stage into the
    // pipeline. We place this stage here so that any $match stages which follow the $changeStream
    // pipeline may be able to skip ahead of the DSCSAddPreImage stage. This allows a whole-db or
    // whole-cluster stream to run on an instance where only some collections have pre-images
    // enabled, so long as the user filters for only those namespaces.
    if (spec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff) {
        stages.push_back(DocumentSourceChangeStreamAddPreImage::create(expCtx, spec));
    }

    // If 'fullDocument' is not set to "default", add the DSCSAddPostImage stage here.
    if (spec.getFullDocument() != FullDocumentModeEnum::kDefault) {
        stages.push_back(DocumentSourceChangeStreamAddPostImage::create(expCtx, spec));
    }

    // If the pipeline is built on router, inject a DSCSHandleTopologyChange stage for v1 change
    // stream readers or a DSC"HandleTopologyChangeV2 stage for v2 change stream readers. The
    // DSCSHandleTopologyChange(V2) stage acts as the split point for the pipeline. All stages
    // before this stage will run on shards and all stages after and inclusive of this stage will
    // run on the router.
    if (expCtx->getInRouter()) {
        if (useV2ChangeStreamReader) {
            // V2 change stream reader, using the HandleTopologyChangeV2 stage.
            stages.push_back(DocumentSourceChangeStreamHandleTopologyChangeV2::create(expCtx));

            // The end-of-transaction synthesis stage sits immediately after the topology stage and
            // is the router-side consumer of the shards' accounting control events. Gate it only on
            // the stream's opt-in: the stage must exist whenever the stream opted in, because it is
            // the only consumer of accounting control events, and those must never reach the user.
            // The 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag is already
            // validated when the '$changeStream' spec is parsed (a stream cannot opt in without
            // it), and the 'allowEndOfTransaction' server parameter is applied where it takes
            // effect (the shard's accounting emission and the planner's user sub-pipeline
            // population), so neither is re-checked here. Re-checking the parameter here would also
            // risk disagreeing with the planner's later check if the parameter is changed in
            // between.
            if (change_stream::showEndOfTransactionRequested(spec)) {
                stages.push_back(DocumentSourceChangeStreamEndOfTransaction::create(expCtx));
            }
        } else {
            // V1 change stream reader, using the HandleTopologyChange stage.
            stages.push_back(DocumentSourceChangeStreamHandleTopologyChange::create(expCtx));
        }
    }

    // If the resume point is an event, we must include a DSCSEnsureResumeTokenPresent stage.
    if (!ResumeToken::isHighWaterMarkToken(resumeToken)) {
        stages.push_back(DocumentSourceChangeStreamEnsureResumeTokenPresent::create(expCtx, spec));
    }

    // If 'showExpandedEvents' is NOT set, add a filter that returns only classic change events.
    if (!spec.getShowExpandedEvents()) {
        // If the stream opted in to endOfTransaction events, include those as well. As with the
        // synthesis stage above, this filter is gated only on the stream's opt-in and does not
        // re-check the 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag or the
        // 'allowEndOfTransaction' server parameter, which are applied where they take effect (the
        // shard's accounting emission and the planner's user sub-pipeline population). Admitting
        // 'endOfTransaction' here is harmless while the feature is disabled, because no such events
        // are produced then.
        stages.push_back(DocumentSourceInternalChangeStreamMatch::create(
            change_stream_filter::getMatchFilterForClassicOperationTypes(
                change_stream::showEndOfTransactionRequested(spec)),
            expCtx));
    }

    return stages;
}

DocumentSourceContainer getEndOfTransactionUserStages(
    const Pipeline& shardsPipeline, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    DocumentSourceContainer userStages;
    for (const auto& stage : shardsPipeline.getSources()) {
        const auto& constraints = stage->constraints();
        // Re-apply only the user stages that are allowlisted for change streams (e.g. $match,
        // $redact and the single-document transformations). Skip the internal change stream
        // stages and the change-stream-only stages such as $changeStreamSplitLargeEvent: the
        // latter runs on the shards and is stateful, so re-running it on the router can reorder
        // or drop synthesized events.
        if (constraints.changeStreamRequirement !=
            StageConstraints::ChangeStreamRequirement::kAllowlist) {
            continue;
        }
        tassert(13766908,
                str::stream() << "unexpected stage '" << stage->getSourceName()
                              << "' would be re-applied to synthesized end-of-transaction events",
                kExpectedUserStages.contains(stage->getSourceName()));
        userStages.push_back(stage->clone(expCtx));
    }
    return userStages;
}

void setEndOfTransactionUserPipeline(const Pipeline& shardsPipeline, Pipeline& mergePipeline) {
    for (const auto& stage : mergePipeline.getSources()) {
        if (auto* endOfTransaction =
                dynamic_cast<DocumentSourceChangeStreamEndOfTransaction*>(stage.get())) {
            endOfTransaction->setUserPipeline(
                getEndOfTransactionUserStages(shardsPipeline, mergePipeline.getContext()));
            return;
        }
    }
}

}  // namespace mongo::change_stream::pipeline_helpers
