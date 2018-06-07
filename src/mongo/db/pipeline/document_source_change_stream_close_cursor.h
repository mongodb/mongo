/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
class DocumentSourceCloseCursor final : public DocumentSource, public NeedsMergerDocumentSource {
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
                (pipeState == Pipeline::SplitState::kUnsplit ? HostTypeRequirement::kNone
                                                             : HostTypeRequirement::kMongoS),
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

    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return nullptr;
    }

    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        // This stage must run on mongos to ensure it sees any invalidation in the correct order,
        // and to ensure that all remote cursors are cleaned up properly. We also must include a
        // mergingPresorted $sort stage to communicate to the AsyncResultsMerger that we need to
        // merge the streams in a particular order.
        const bool mergingPresorted = true;
        const long long noLimit = -1;
        auto sortMergingPresorted =
            DocumentSourceSort::create(pExpCtx,
                                       change_stream_constants::kSortSpec,
                                       noLimit,
                                       DocumentSourceSort::kMaxMemoryUsageBytes,
                                       mergingPresorted);
        return {sortMergingPresorted, this};
    }

private:
    /**
     * Use the create static method to create a DocumentSourceCloseCursor.
     */
    DocumentSourceCloseCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}

    bool _shouldCloseCursor = false;
    boost::optional<Document> _queuedInvalidate;
};

}  // namespace mongo
