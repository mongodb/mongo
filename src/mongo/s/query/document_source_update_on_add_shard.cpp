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
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/async_results_merger_params_gen.h"
#include "mongo/s/query/establish_cursors.h"

namespace mongo {
namespace {

// Returns true if the change stream document has an 'operationType' of 'newShardDetected'.
bool needsUpdate(const Document& childResult) {
    return childResult[DocumentSourceChangeStream::kOperationTypeField].getStringData() ==
        DocumentSourceChangeStream::kNewShardDetectedOpType;
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
    : DocumentSource(expCtx),
      _executor(std::move(executor)),
      _mergeCursors(mergeCursors),
      _shardsWithCursors(std::move(shardsWithCursors)),
      _cmdToRunOnNewShards(cmdToRunOnNewShards.getOwned()) {}

DocumentSource::GetNextResult DocumentSourceUpdateOnAddShard::getNext() {
    auto childResult = pSource->getNext();

    while (childResult.isAdvanced() && needsUpdate(childResult.getDocument())) {
        addNewShardCursors(childResult.getDocument());
        childResult = pSource->getNext();
    }
    return childResult;
}

void DocumentSourceUpdateOnAddShard::addNewShardCursors(const Document& newShardDetectedObj) {
    _mergeCursors->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor> DocumentSourceUpdateOnAddShard::establishShardCursorsOnNewShards(
    const Document& newShardDetectedObj) {
    auto* opCtx = pExpCtx->opCtx;
    // Reload the shard registry. We need to ensure a reload initiated after calling this method
    // caused the reload, otherwise we aren't guaranteed to get all the new shards.
    auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
    if (!shardRegistry->reload(opCtx)) {
        // A 'false' return from shardRegistry.reload() means a reload was already in progress and
        // it completed before reload() returned. So another reload(), regardless of return value,
        // will ensure a reload started after the first call to reload().
        shardRegistry->reload(opCtx);
    }

    std::vector<ShardId> shardIds, newShardIds;
    shardRegistry->getAllShardIdsNoReload(&shardIds);
    std::sort(_shardsWithCursors.begin(), _shardsWithCursors.end());
    std::sort(shardIds.begin(), shardIds.end());
    std::set_difference(shardIds.begin(),
                        shardIds.end(),
                        _shardsWithCursors.begin(),
                        _shardsWithCursors.end(),
                        std::back_inserter(newShardIds));

    auto cmdObj = DocumentSourceChangeStream::replaceResumeTokenInCommand(
        _cmdToRunOnNewShards,
        newShardDetectedObj[DocumentSourceChangeStream::kIdField].getDocument());
    std::vector<std::pair<ShardId, BSONObj>> requests;
    for (const auto& shardId : newShardIds) {
        requests.emplace_back(shardId, cmdObj);
        _shardsWithCursors.push_back(shardId);
    }
    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            _executor,
                            pExpCtx->ns,
                            ReadPreferenceSetting::get(opCtx),
                            requests,
                            allowPartialResults);
}

}  // namespace mongo
