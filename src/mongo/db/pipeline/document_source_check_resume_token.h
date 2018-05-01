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

#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/resume_token.h"

namespace mongo {
/**
 * This checks for resumability on a single shard in the sharded case. The rules are
 *
 * - If the first document in the pipeline for this shard has a matching timestamp, we can
 *   always resume.
 * - If the oplog is empty, we can resume.  An empty oplog is rare and can only occur
 *   on a secondary that has just started up from a primary that has not taken a write.
 *   In particular, an empty oplog cannot be the result of oplog truncation.
 * - If neither of the above is true, the least-recent document in the oplog must precede the resume
 *   timestamp. If we do this check after seeing the first document in the pipeline in the shard, or
 *   after seeing that there are no documents in the pipeline after the resume token in the shard,
 *   we're guaranteed not to miss any documents.
 *
 * - Otherwise we cannot resume, as we do not know if this shard lost documents between the resume
 *   token and the first matching document in the pipeline.
 *
 * This source need only run on a sharded collection.  For unsharded collections,
 * DocumentSourceEnsureResumeTokenPresent is sufficient.
 */
class DocumentSourceShardCheckResumability final : public DocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSourceShardCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Timestamp ts);

private:
    /**
     * Use the create static method to create a DocumentSourceShardCheckResumability.
     */
    DocumentSourceShardCheckResumability(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         Timestamp ts);

    Timestamp _resumeTimestamp;
    bool _verifiedResumability;
};

/**
 * This stage is used internally for change streams to ensure that the resume token is in the
 * stream.  It is not intended to be created by the user.
 */
class DocumentSourceEnsureResumeTokenPresent final : public DocumentSource,
                                                     public NeedsMergerDocumentSource {
public:
    // Used to record the results of comparing the token data extracted from documents in the
    // resumed stream against the client's resume token.
    enum class ResumeStatus {
        kFoundToken,    // The stream produced a document satisfying the client resume token.
        kCannotResume,  // The stream's latest document is more recent than the resume token.
        kCheckNextDoc   // The next document produced by the stream may contain the resume token.
    };

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
                TransactionRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    /**
     * NeedsMergerDocumentSource methods; this has to run on the merger, since the resume point
     * could be at any shard. Also add a DocumentSourceShardCheckResumability stage on the shards
     * pipeline to ensure that each shard has enough oplog history to resume the change stream.
     */
    boost::intrusive_ptr<DocumentSource> getShardSource() final {
        return DocumentSourceShardCheckResumability::create(pExpCtx,
                                                            _tokenFromClient.getClusterTime());
    };

    std::list<boost::intrusive_ptr<DocumentSource>> getMergeSources() final {
        // This stage must run on mongos to ensure it sees the resume token, which could have come
        // from any shard.  We also must include a mergingPresorted $sort stage to communicate to
        // the AsyncResultsMerger that we need to merge the streams in a particular order.
        const bool mergingPresorted = true;
        const long long noLimit = -1;
        auto sortMergingPresorted =
            DocumentSourceSort::create(pExpCtx,
                                       change_stream_constants::kSortSpec,
                                       noLimit,
                                       DocumentSourceSort::kMaxMemoryUsageBytes,
                                       mergingPresorted);
        return {sortMergingPresorted, this};
    };

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSourceEnsureResumeTokenPresent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeToken token);

    const ResumeToken& getTokenForTest() {
        return _tokenFromClient;
    }

private:
    /**
     * Use the create static method to create a DocumentSourceEnsureResumeTokenPresent.
     */
    DocumentSourceEnsureResumeTokenPresent(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           ResumeToken token);

    ResumeStatus _resumeStatus = ResumeStatus::kCheckNextDoc;
    ResumeToken _tokenFromClient;
};

}  // namespace mongo
