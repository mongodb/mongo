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

#pragma once

#include <memory>

#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/shard_id.h"

namespace mongo {

/**
 * An internal stage used as part of the change streams infrastructure to listen for an event
 * signaling that a new shard now has potentially matching data. For example, this stage will
 * detect if a collection is being watched and a chunk for that collection migrates to a shard for
 * the first time. When this event is detected, this stage will establish a new cursor on that
 * shard and add it to the cursors being merged.
 */
class DocumentSourceChangeStreamHandleTopologyChange final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamHandleTopologyChange"_sd;

    /**
     * Creates a new stage which will establish a new cursor and add it to the cursors being merged
     * by 'mergeCursorsStage' whenever a new shard is detected by a change stream.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final;

    StageConstraints constraints(Pipeline::SplitState) const final;

    GetModPathsReturn getModifiedPaths() const final {
        // This stage neither modifies nor renames any field.
        return {GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{nullptr, this, change_stream_constants::kSortSpec};
    }

private:
    DocumentSourceChangeStreamHandleTopologyChange(const boost::intrusive_ptr<ExpressionContext>&);

    GetNextResult doGetNext() final;

    /**
     * Establish the new cursors and tell the RouterStageMerge about them.
     */
    void addNewShardCursors(const Document& newShardDetectedObj);

    /**
     * Open the cursors on the new shards.
     */
    std::vector<RemoteCursor> establishShardCursorsOnNewShards(const Document& newShardDetectedObj);

    /**
     * Updates the $changeStream stage in the '_originalAggregateCommand' to reflect the start time
     * for the newly-added shard(s), then generates the final command object to be run on those
     * shards.
     */
    BSONObj createUpdatedCommandForNewShard(Timestamp shardAddedTime);

    /**
     * Given the '_originalAggregateCommand' and a resume token, returns a new BSON object with the
     * same command except with the addition of a resumeAfter option containing the resume token.
     * If there was a previous resumeAfter option, it will be removed.
     */
    BSONObj replaceResumeTokenInCommand(Document resumeToken);

    boost::intrusive_ptr<DocumentSourceMergeCursors> _mergeCursors;
    BSONObj _originalAggregateCommand;
};
}  // namespace mongo
