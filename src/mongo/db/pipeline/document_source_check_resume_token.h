/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/resume_token.h"

namespace mongo {
/**
 * This checks for resumability on a single shard in the sharded case. The rules are
 *
 * - If the first document in the pipeline for this shard has a matching resume token, we can
 *   always resume.
 * - If the oplog is empty, we can resume.  An empty oplog is rare and can only occur
 *   on a secondary that has just started up from a primary that has not taken a write.
 *   In particular, an empty oplog cannot be the result of oplog truncation.
 * - If neither of the above is true, the least-recent document in the oplog must precede the resume
 *   token.  If we do this check after seeing the first document in the pipeline in the shard, or
 *   after seeing that there are no documents in the pipeline after the resume token in the shard,
 *   we're guaranteed not to miss any documents.
 *
 * - Otherwise we cannot resume, as we do not know if this shard lost documents between the resume
 *   token and the first matching document in the pipeline.
 *
 * This source need only run on a sharded collection.  For unsharded collections,
 * DocumentSourceEnsureResumeTokenPresent is sufficient.
 */
class DocumentSourceShardCheckResumability final : public DocumentSourceNeedsMongoProcessInterface {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSourceShardCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token);

private:
    /**
     * Use the create static method to create a DocumentSourceShardCheckResumability.
     */
    DocumentSourceShardCheckResumability(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         ResumeToken token);

    ResumeToken _token;
    bool _verifiedResumability;
};

/**
 * This stage is used internally for change streams to ensure that the resume token is in the
 * stream.  It is not intended to be created by the user.
 */
class DocumentSourceEnsureResumeTokenPresent final : public DocumentSource,
                                                     public SplittableDocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        // This stage should never be in the shards part of a split pipeline.
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                (pipeState == Pipeline::SplitState::kUnsplit ? HostTypeRequirement::kNone
                                                             : HostTypeRequirement::kMongoS),
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    /**
     * SplittableDocumentSource methods; this has to run on the merger, since the resume point could
     * be at any shard.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return DocumentSourceShardCheckResumability::create(pExpCtx, _token);
    };

    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        // This stage must run on mongos to ensure it sees the resume token, which could have come
        // from any shard.  We also must include a mergingPresorted $sort stage to communicate to
        // the AsyncResultsMerger that we need to merge the streams in a particular order.
        const bool mergingPresorted = true;
        const long long noLimit = -1;
        auto sortMergingPresorted =
            DocumentSourceSort::create(pExpCtx,
                                       DocumentSourceChangeStream::kSortSpec,
                                       noLimit,
                                       DocumentSourceSort::kMaxMemoryUsageBytes,
                                       mergingPresorted);
        return {sortMergingPresorted, this};
    };

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSourceEnsureResumeTokenPresent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token);

    const ResumeToken& getTokenForTest() {
        return _token;
    }

private:
    /**
     * Use the create static method to create a DocumentSourceEnsureResumeTokenPresent.
     */
    DocumentSourceEnsureResumeTokenPresent(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           ResumeToken token);

    ResumeToken _token;
    bool _haveSeenResumeToken;
};

}  // namespace mongo
