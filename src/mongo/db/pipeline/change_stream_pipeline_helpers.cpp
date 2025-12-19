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

#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"

#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"
#include "mongo/db/pipeline/document_source_change_stream_inject_control_events.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/util/assert_util.h"

#include <list>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::change_stream::pipeline_helpers {
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
        stages.push_back(DocumentSourceInternalChangeStreamMatch::create(
            change_stream_filter::getMatchFilterForClassicOperationTypes(), expCtx));
    }

    return stages;
}

}  // namespace mongo::change_stream::pipeline_helpers
