// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_topology_helpers.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::change_stream::topology_helpers {

BSONObj replaceResumeTokenAndVersionInCommand(
    Document resumeToken,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion) {
    Document originalCmd(originalAggregateCommand);
    auto pipeline = originalCmd[AggregateCommandRequest::kPipelineFieldName].getArray();

    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    tassert(5549102,
            "Invalid $changeStream command object",
            !pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());

    if (changeStreamVersion.has_value()) {
        changeStreamStage[DocumentSourceChangeStreamSpec::kVersionFieldName] =
            Value(idl::serialize(*changeStreamVersion));
    }

    // Provide 'resumeToken' as part 'startAfter' for resuming the changeStream. 'startAfter' and
    // not 'resumeAfter' attribute is provided, to allow opening the change stream cursors  with an
    // invalidation 'resumeToken' resume token.
    // All other forms of resuming the change stream are set to null if provided in the original
    // command.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAfterFieldName] = Value(resumeToken);
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] = Value();
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value();

    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(std::move(originalCmd));
    newCmd[AggregateCommandRequest::kPipelineFieldName] = Value(std::move(pipeline));
    return newCmd.freeze().toBson();
}

BSONObj createUpdatedCommandForNewShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp atClusterTime,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion) {
    return createUpdatedCommandForNewShard(
        expCtx,
        ResumeToken::makeHighWaterMarkToken(atClusterTime, expCtx->getChangeStreamTokenVersion()),
        originalAggregateCommand,
        changeStreamVersion);
}

BSONObj createUpdatedCommandForNewShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ResumeToken& resumeTokenForNewShard,
    const BSONObj& originalAggregateCommand,
    const boost::optional<ChangeStreamReaderVersionEnum>& changeStreamVersion) {
    // Create a new shard command object containing the new resume token, adding the 'resumeAfter'
    // field to the '$changeStream' spec. An example input 'originalAggregateCommand' is:
    // {"aggregate":"test","pipeline":[{"$changeStream":{"fullDocument":"default"}}],"cursor":{"batchSize":101}}
    BSONObj shardCommand = replaceResumeTokenAndVersionInCommand(
        resumeTokenForNewShard.toDocument(), originalAggregateCommand, changeStreamVersion);

    tassert(7663502,
            str::stream() << "SerializationContext on the expCtx should not be empty, with ns: "
                          << expCtx->getNamespaceString().toStringForErrorMsg(),
            expCtx->getSerializationContext() != SerializationContext::stateDefault());

    // Parse and optimize the pipeline. This will also insert all internal change stream stages into
    // the pipeline.
    pipeline_factory::MakePipelineOptions opts{.alreadyOptimized = false,
                                               .attachCursorSource = false};

    auto pipeline = pipeline_factory::makePipeline(
        shardCommand[AggregateCommandRequest::kPipelineFieldName], expCtx, opts);

    // Split the full pipeline to get the shard pipeline.
    auto splitPipelines = sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));

    // Create the new command that will run on the shard.
    return sharded_agg_helpers::createCommandForTargetedShards(expCtx,
                                                               Document{shardCommand},
                                                               splitPipelines,
                                                               boost::none, /* exchangeSpec */
                                                               true /* needsMerge */,
                                                               boost::none /* explain */);
}

}  // namespace mongo::change_stream::topology_helpers
