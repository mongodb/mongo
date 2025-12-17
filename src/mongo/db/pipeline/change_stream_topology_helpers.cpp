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

#include "mongo/db/pipeline/change_stream_topology_helpers.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/optimization/optimize.h"
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
            Value(ChangeStreamReaderVersion_serializer(*changeStreamVersion));
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
