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

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>
#include <string>
#include <utility>

#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/change_stream_topology_change_info.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamHandleTopologyChange,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamHandleTopologyChange::createFromBson,
                                  true);

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
    if (opType.getType() != BSONType::String) {
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
    const bool isConfigDotShardsEvent = nsObj["db"_sd].getType() == BSONType::String &&
        nsObj["db"_sd].getStringData() ==
            NamespaceString::kConfigsvrShardsNamespace.db(omitTenant) &&
        nsObj["coll"_sd].getType() == BSONType::String &&
        nsObj["coll"_sd].getStringData() == NamespaceString::kConfigsvrShardsNamespace.coll();

    // If it isn't from config.shards, treat it as a normal user event.
    if (!isConfigDotShardsEvent) {
        return false;
    }

    // We need to validate that this event hasn't been faked by a user projection in a way that
    // would cause us to tassert. Check the clusterTime field, which is needed to determine the
    // point from which the new shard should start reporting change events.
    if (eventDoc["clusterTime"].getType() != BSONType::bsonTimestamp) {
        return false;
    }
    // Check the fullDocument field, which should contain details of the new shard's name and hosts.
    auto fullDocument = eventDoc[DocumentSourceChangeStream::kFullDocumentField];
    if (opType.getStringData() == "insert"_sd && fullDocument.getType() != BSONType::Object) {
        return false;
    }

    // The event is on config.shards and is well-formed. It is still possible that it is a forgery,
    // but all the user can do is cause their own stream to uassert.
    return true;
}
}  // namespace

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange>
DocumentSourceChangeStreamHandleTopologyChange::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(8131300,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == Object && elem.Obj().isEmpty());
    return new DocumentSourceChangeStreamHandleTopologyChange(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange>
DocumentSourceChangeStreamHandleTopologyChange::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamHandleTopologyChange(expCtx);
}

DocumentSourceChangeStreamHandleTopologyChange::DocumentSourceChangeStreamHandleTopologyChange(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

StageConstraints DocumentSourceChangeStreamHandleTopologyChange::constraints(
    Pipeline::SplitState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kMongoS,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage};

    // Can be swapped with the '$match', '$redact', and 'DocumentSourceSingleDocumentTransformation'
    // stages and ensures that they get pushed down to the shards, as this stage bisects the change
    // streams pipeline.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransformOrRedact = true;

    return constraints;
}

DocumentSource::GetNextResult DocumentSourceChangeStreamHandleTopologyChange::doGetNext() {
    // For the first call to the 'doGetNext', the '_mergeCursors' will be null and must be
    // populated. We also resolve the original aggregation command from the expression context.
    if (!_mergeCursors) {
        _mergeCursors = dynamic_cast<DocumentSourceMergeCursors*>(pSource);
        _originalAggregateCommand = pExpCtx->originalAggregateCommand.getOwned();

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

void DocumentSourceChangeStreamHandleTopologyChange::addNewShardCursors(
    const Document& newShardDetectedObj) {
    _mergeCursors->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor>
DocumentSourceChangeStreamHandleTopologyChange::establishShardCursorsOnNewShards(
    const Document& newShardDetectedObj) {
    // Reload the shard registry to see the new shard.
    auto* opCtx = pExpCtx->opCtx;
    Grid::get(opCtx)->shardRegistry()->reload(opCtx);

    // Parse the new shard's information from the document inserted into 'config.shards'.
    auto newShardSpec = newShardDetectedObj[DocumentSourceChangeStream::kFullDocumentField];
    auto newShard = uassertStatusOK(ShardType::fromBSON(newShardSpec.getDocument().toBson()));

    // Make sure we are not attempting to open a cursor on a shard that already has one.
    if (_mergeCursors->getShardIds().count(newShard.getName()) != 0) {
        return {};
    }

    auto cmdObj = createUpdatedCommandForNewShard(
        newShardDetectedObj[DocumentSourceChangeStream::kClusterTimeField].getTimestamp());

    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            pExpCtx->mongoProcessInterface->taskExecutor,
                            pExpCtx->ns,
                            ReadPreferenceSetting::get(opCtx),
                            {{newShard.getName(), cmdObj}},
                            allowPartialResults);
}

BSONObj DocumentSourceChangeStreamHandleTopologyChange::createUpdatedCommandForNewShard(
    Timestamp shardAddedTime) {
    // We must start the new cursor from the moment at which the shard became visible.
    const auto newShardAddedTime = LogicalTime{shardAddedTime};
    auto resumeTokenForNewShard = ResumeToken::makeHighWaterMarkToken(
        newShardAddedTime.addTicks(1).asTimestamp(), pExpCtx->changeStreamTokenVersion);

    // Create a new shard command object containing the new resume token.
    auto shardCommand = replaceResumeTokenInCommand(resumeTokenForNewShard.toDocument());

    auto* opCtx = pExpCtx->opCtx;
    bool apiStrict = APIParameters::get(opCtx).getAPIStrict().value_or(false);

    tassert(7663502,
            str::stream() << "SerializationContext on the expCtx should not be empty, with ns: "
                          << pExpCtx->ns.toStringForErrorMsg(),
            pExpCtx->serializationCtxt != SerializationContext::stateDefault());

    // Create the 'AggregateCommandRequest' object which will help in creating the parsed pipeline.
    auto aggCmdRequest = aggregation_request_helper::parseFromBSON(
        opCtx, pExpCtx->ns, shardCommand, boost::none, apiStrict, pExpCtx->serializationCtxt);

    // Parse and optimize the pipeline.
    auto pipeline = Pipeline::parse(aggCmdRequest.getPipeline(), pExpCtx);
    pipeline->optimizePipeline();

    // Split the full pipeline to get the shard pipeline.
    auto splitPipelines = sharded_agg_helpers::splitPipeline(std::move(pipeline));

    // Create the new command that will run on the shard.
    return sharded_agg_helpers::createCommandForTargetedShards(pExpCtx,
                                                               Document{shardCommand},
                                                               splitPipelines,
                                                               boost::none, /* exhangeSpec */
                                                               true /* needsMerge */,
                                                               boost::none /* explain */);
}

BSONObj DocumentSourceChangeStreamHandleTopologyChange::replaceResumeTokenInCommand(
    Document resumeToken) {
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

Value DocumentSourceChangeStreamHandleTopologyChange::serialize(
    const SerializationOptions& opts) const {
    if (opts.verbosity) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage"
                                << "internalHandleTopologyChange"_sd)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
