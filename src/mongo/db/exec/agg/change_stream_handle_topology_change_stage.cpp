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

#include "mongo/db/exec/agg/change_stream_handle_topology_change_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_topology_change_info.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/sharding_environment/grid.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamHandleTopologyChangeToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamHandleTopologyChangeDS =
        dynamic_cast<DocumentSourceChangeStreamHandleTopologyChange*>(documentSource.get());

    tassert(10561309,
            "expected 'DocumentSourceChangeStreamHandleTopologyChange' type",
            changeStreamHandleTopologyChangeDS);

    return make_intrusive<exec::agg::ChangeStreamHandleTopologyChangeStage>(
        changeStreamHandleTopologyChangeDS->kStageName,
        changeStreamHandleTopologyChangeDS->getExpCtx());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamHandleTopologyChange,
                           DocumentSourceChangeStreamHandleTopologyChange::id,
                           documentSourceChangeStreamHandleTopologyChangeToStageFn)

namespace {
// Failpoint to throw an exception when the 'kNewShardDetected' event is observed.
MONGO_FAIL_POINT_DEFINE(throwChangeStreamTopologyChangeExceptionToClient);

// Returns true if the change stream document is an event in 'config.shards'.
bool isShardConfigEvent(const Document& eventDoc) {
    // TODO SERVER-44039: we continue to generate 'kNewShardDetected' events for compatibility
    // with 4.2, even though we no longer rely on them to detect new shards. We swallow the event
    // here. We may wish to remove this mechanism entirely in 4.7+, or retain it for future cases
    // where a change stream is targeted to a subset of shards. See SERVER-44039 for details.

    auto opType = eventDoc[DocumentSourceChangeStream::kOperationTypeField];

    // If opType isn't a string, then this document has been manipulated. This means it cannot have
    // been produced by the internal shard-monitoring cursor that we opened on the config servers,
    // or by the kNewShardDetectedOpType mechanism, which bypasses filtering and projection stages.
    if (opType.getType() != BSONType::string) {
        return false;
    }

    if (opType.getStringData() == DocumentSourceChangeStream::kNewShardDetectedOpType) {
        // If the failpoint is enabled, throw the 'ChangeStreamToplogyChange' exception to the
        // client. This is used in testing to confirm that the swallowed 'kNewShardDetected' event
        // has reached the mongoS.
        // TODO SERVER-30784: remove this failpoint when the 'kNewShardDetected' event is the only
        // way we detect a new shard.
        if (MONGO_unlikely(throwChangeStreamTopologyChangeExceptionToClient.shouldFail())) {
            uasserted(ChangeStreamTopologyChangeInfo(eventDoc.toBsonWithMetaData()),
                      "Collection migrated to new shard");
        }

        return true;
    }

    // Check whether this event occurred on the config.shards collection.
    auto nsObj = eventDoc[DocumentSourceChangeStream::kNamespaceField];
    const bool isConfigDotShardsEvent = nsObj["db"_sd].getType() == BSONType::string &&
        nsObj["db"_sd].getStringData() ==
            NamespaceString::kConfigsvrShardsNamespace.db(omitTenant) &&
        nsObj["coll"_sd].getType() == BSONType::string &&
        nsObj["coll"_sd].getStringData() == NamespaceString::kConfigsvrShardsNamespace.coll();

    // If it isn't from config.shards, treat it as a normal user event.
    if (!isConfigDotShardsEvent) {
        return false;
    }

    // We need to validate that this event hasn't been faked by a user projection in a way that
    // would cause us to tassert. Check the clusterTime field, which is needed to determine the
    // point from which the new shard should start reporting change events.
    if (eventDoc["clusterTime"].getType() != BSONType::timestamp) {
        return false;
    }
    // Check the fullDocument field, which should contain details of the new shard's name and hosts.
    auto fullDocument = eventDoc[DocumentSourceChangeStream::kFullDocumentField];
    if (opType.getStringData() == "insert"_sd && fullDocument.getType() != BSONType::object) {
        return false;
    }

    // The event is on config.shards and is well-formed. It is still possible that it is a forgery,
    // but all the user can do is cause their own stream to uassert.
    return true;
}
}  // namespace

ChangeStreamHandleTopologyChangeStage::ChangeStreamHandleTopologyChangeStage(
    StringData stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : Stage(stageName, pExpCtx) {}

GetNextResult ChangeStreamHandleTopologyChangeStage::doGetNext() {
    // For the first call to the 'doGetNext', the '_mergeCursors' will be null and must be
    // populated. We also resolve the original aggregation command from the expression context.
    if (!_mergeCursors) {
        _mergeCursors = dynamic_cast<MergeCursorsStage*>(pSource);
        _originalAggregateCommand = pExpCtx->getOriginalAggregateCommand().getOwned();

        tassert(5549100, "Missing $mergeCursors stage", _mergeCursors);
        tassert(
            5549101, "Empty $changeStream command object", !_originalAggregateCommand.isEmpty());
    }

    auto childResult = pSource->getNext();

    // If this is an insertion into the 'config.shards' collection, open a cursor on the new shard.
    while (childResult.isAdvanced() && isShardConfigEvent(childResult.getDocument())) {
        auto opType = childResult.getDocument()[DocumentSourceChangeStream::kOperationTypeField];
        if (opType.getStringData() == DocumentSourceChangeStream::kInsertOpType) {
            addNewShardCursors(childResult.getDocument());
        }
        // For shard removal or update, we do nothing. We also swallow kNewShardDetectedOpType.
        childResult = pSource->getNext();
    }
    return childResult;
}

void ChangeStreamHandleTopologyChangeStage::addNewShardCursors(
    const Document& newShardDetectedObj) {
    _mergeCursors->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor> ChangeStreamHandleTopologyChangeStage::establishShardCursorsOnNewShards(
    const Document& newShardDetectedObj) {
    // Reload the shard registry to see the new shard.
    auto* opCtx = pExpCtx->getOperationContext();
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    // Parse the new shard's information from the document inserted into 'config.shards'.
    auto newShardSpec = newShardDetectedObj[DocumentSourceChangeStream::kFullDocumentField];
    auto newShard = uassertStatusOK(ShardType::fromBSON(newShardSpec.getDocument().toBson()));

    // Make sure we are not attempting to open a cursor on a shard that already has one.
    if (_mergeCursors->hasShardId(newShard.getName())) {
        return {};
    }

    auto cmdObj = createUpdatedCommandForNewShard(
        newShardDetectedObj[DocumentSourceChangeStream::kClusterTimeField].getTimestamp());

    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            pExpCtx->getMongoProcessInterface()->taskExecutor,
                            pExpCtx->getNamespaceString(),
                            ReadPreferenceSetting::get(opCtx),
                            {{newShard.getName(), cmdObj}},
                            allowPartialResults);
}


BSONObj ChangeStreamHandleTopologyChangeStage::createUpdatedCommandForNewShard(
    Timestamp shardAddedTime) {
    // We must start the new cursor from the moment at which the shard became visible.
    const auto newShardAddedTime = LogicalTime{shardAddedTime};
    auto resumeTokenForNewShard = ResumeToken::makeHighWaterMarkToken(
        newShardAddedTime.addTicks(1).asTimestamp(), pExpCtx->getChangeStreamTokenVersion());

    // Create a new shard command object containing the new resume token.
    auto shardCommand = replaceResumeTokenInCommand(resumeTokenForNewShard.toDocument());

    tassert(7663502,
            str::stream() << "SerializationContext on the expCtx should not be empty, with ns: "
                          << pExpCtx->getNamespaceString().toStringForErrorMsg(),
            pExpCtx->getSerializationContext() != SerializationContext::stateDefault());

    // Parse and optimize the pipeline.
    auto pipeline = Pipeline::parseFromArray(
        shardCommand[AggregateCommandRequest::kPipelineFieldName], pExpCtx);
    pipeline->optimizePipeline();

    // Split the full pipeline to get the shard pipeline.
    auto splitPipelines = sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));

    // Create the new command that will run on the shard.
    return sharded_agg_helpers::createCommandForTargetedShards(pExpCtx,
                                                               Document{shardCommand},
                                                               splitPipelines,
                                                               boost::none, /* exhangeSpec */
                                                               true /* needsMerge */,
                                                               boost::none /* explain */);
}

BSONObj ChangeStreamHandleTopologyChangeStage::replaceResumeTokenInCommand(Document resumeToken) {
    Document originalCmd(_originalAggregateCommand);
    auto pipeline = originalCmd[AggregateCommandRequest::kPipelineFieldName].getArray();

    // A $changeStream must be the first element of the pipeline in order to be able
    // to replace (or add) a resume token.
    tassert(5549102,
            "Invalid $changeStream command object",
            !pipeline[0][DocumentSourceChangeStream::kStageName].missing());

    MutableDocument changeStreamStage(
        pipeline[0][DocumentSourceChangeStream::kStageName].getDocument());
    changeStreamStage[DocumentSourceChangeStreamSpec::kResumeAfterFieldName] = Value(resumeToken);

    // If the command was initially specified with a startAtOperationTime, we need to remove it to
    // use the new resume token.
    changeStreamStage[DocumentSourceChangeStreamSpec::kStartAtOperationTimeFieldName] = Value();
    pipeline[0] =
        Value(Document{{DocumentSourceChangeStream::kStageName, changeStreamStage.freeze()}});
    MutableDocument newCmd(std::move(originalCmd));
    newCmd[AggregateCommandRequest::kPipelineFieldName] = Value(std::move(pipeline));
    return newCmd.freeze().toBson();
}

}  // namespace exec::agg
}  // namespace mongo
