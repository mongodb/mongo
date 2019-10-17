/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/query/document_source_update_on_add_shard.h"

#include <algorithm>

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/establish_cursors.h"

namespace mongo {
namespace {

// Returns true if the change stream document is an event in 'config.shards'.
bool isShardConfigEvent(const Document& eventDoc) {
    // TODO SERVER-44039: we continue to generate 'kNewShardDetected' events for compatibility
    // with 4.2, even though we no longer rely on them to detect new shards. We swallow the event
    // here. We may wish to remove this mechanism entirely 4.6, or retain it for future cases where
    // a change stream is targeted to a subset of shards. See SERVER-44039 for details.
    if (eventDoc[DocumentSourceChangeStream::kOperationTypeField].getStringData() ==
        DocumentSourceChangeStream::kNewShardDetectedOpType) {
        return true;
    }
    auto nsObj = eventDoc[DocumentSourceChangeStream::kNamespaceField];
    return nsObj.getType() == BSONType::Object &&
        nsObj["db"_sd].getStringData() == ShardType::ConfigNS.db() &&
        nsObj["coll"_sd].getStringData() == ShardType::ConfigNS.coll();
}
}  // namespace

boost::intrusive_ptr<DocumentSourceUpdateOnAddShard> DocumentSourceUpdateOnAddShard::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const boost::intrusive_ptr<DocumentSourceMergeCursors>& mergeCursors,
    std::vector<ShardId> shardsWithCursors,
    BSONObj cmdToRunOnNewShards) {
    return new DocumentSourceUpdateOnAddShard(expCtx,
                                              std::move(executor),
                                              mergeCursors,
                                              std::move(shardsWithCursors),
                                              cmdToRunOnNewShards);
}

DocumentSourceUpdateOnAddShard::DocumentSourceUpdateOnAddShard(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const boost::intrusive_ptr<DocumentSourceMergeCursors>& mergeCursors,
    std::vector<ShardId>&& shardsWithCursors,
    BSONObj cmdToRunOnNewShards)
    : DocumentSource(kStageName, expCtx),
      _executor(std::move(executor)),
      _mergeCursors(mergeCursors),
      _shardsWithCursors(shardsWithCursors.begin(), shardsWithCursors.end()),
      _cmdToRunOnNewShards(cmdToRunOnNewShards.getOwned()) {}

DocumentSource::GetNextResult DocumentSourceUpdateOnAddShard::doGetNext() {
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

void DocumentSourceUpdateOnAddShard::addNewShardCursors(const Document& newShardDetectedObj) {
    _mergeCursors->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor> DocumentSourceUpdateOnAddShard::establishShardCursorsOnNewShards(
    const Document& newShardDetectedObj) {
    // Reload the shard registry. We need to ensure a reload initiated after calling this method
    // caused the reload, otherwise we may not see the new shard, so we perform a "hard" reload.
    auto* opCtx = pExpCtx->opCtx;
    if (!Grid::get(opCtx)->shardRegistry()->reload(opCtx)) {
        // A 'false' return from shardRegistry.reload() means a reload was already in progress and
        // it completed before reload() returned. So another reload(), regardless of return value,
        // will ensure a reload started after the first call to reload().
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);
    }

    // Parse the new shard's information from the document inserted into 'config.shards'.
    auto newShardSpec = newShardDetectedObj[DocumentSourceChangeStream::kFullDocumentField];
    auto newShard = uassertStatusOK(ShardType::fromBSON(newShardSpec.getDocument().toBson()));

    // Make sure we are not attempting to open a cursor on a shard that already has one.
    if (!_shardsWithCursors.insert(newShard.getName()).second) {
        return {};
    }

    // We must start the new cursor from the moment at which the shard became visible.
    const auto newShardAddedTime = LogicalTime{
        newShardDetectedObj[DocumentSourceChangeStream::kClusterTimeField].getTimestamp()};
    auto resumeTokenForNewShard =
        ResumeToken::makeHighWaterMarkToken(newShardAddedTime.addTicks(1).asTimestamp());
    auto cmdObj = DocumentSourceChangeStream::replaceResumeTokenInCommand(
        _cmdToRunOnNewShards, resumeTokenForNewShard.toDocument());

    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            _executor,
                            pExpCtx->ns,
                            ReadPreferenceSetting::get(opCtx),
                            {{newShard.getName(), cmdObj}},
                            allowPartialResults);
}

}  // namespace mongo
