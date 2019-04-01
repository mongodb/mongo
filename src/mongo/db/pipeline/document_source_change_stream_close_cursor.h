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

#pragma once

#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_sort.h"

namespace mongo {

/**
 * This stage is used internally for change notifications to close cursor after returning
 * "invalidate" entries.
 * It is not intended to be created by the user.
 */
class DocumentSourceCloseCursor final : public DocumentSource {
public:
    GetNextResult getNext() final;

    const char* getSourceName() const final {
        // This is used in error reporting.
        return "$changeStream";
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // This stage should never be in the shards part of a split pipeline.
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                // If this is parsed on mongos it should stay on mongos. If we're not in a sharded
                // cluster then it's okay to run on mongod.
                HostTypeRequirement::kLocalOnly,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // This stage is created by the DocumentSourceChangeStream stage, so serializing it
        // here would result in it being created twice.
        return Value();
    }

    static boost::intrusive_ptr<DocumentSourceCloseCursor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCloseCursor(expCtx);
    }

    boost::optional<MergingLogic> mergingLogic() final {
        // This stage must run on mongos to ensure it sees any invalidation in the correct order,
        // and to ensure that all remote cursors are cleaned up properly.
        // {shardsStage, mergingStage, sortPattern}
        return MergingLogic{nullptr, this, change_stream_constants::kSortSpec};
    }

    void acceptVisitor(DocumentSourceVisitor* visitor) final {
        visitor->visit(this);
    }

private:
    /**
     * Use the create static method to create a DocumentSourceCloseCursor.
     */
    DocumentSourceCloseCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}

    bool _shouldCloseCursor = false;
};

}  // namespace mongo
