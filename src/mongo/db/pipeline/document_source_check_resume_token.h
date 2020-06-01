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
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {
/**
 * This stage checks whether or not the oplog has enough history to resume the stream, and consumes
 * all events up to the given resume point. It is deployed on all shards when resuming a stream on
 * a sharded cluster, and is also used in the single-replicaset case when a stream is opened with
 * startAtOperationTime or with a high-water-mark resume token. It defers to the COLLSCAN to check
 * whether the first event (matching or non-matching) encountered in the oplog has a timestamp equal
 * to or earlier than the minTs in the change stream filter. If not, the COLLSCAN will throw an
 * assertion, which this stage catches and converts into a more comprehensible $changeStream
 * specific exception. The rules are:
 *
 * - If the first event seen in the oplog has the same timestamp as the requested resume token or
 *   startAtOperationTime, we can resume.
 * - If the timestamp of the first event seen in the oplog is earlier than the requested resume
 *   token or startAtOperationTime, we can resume.
 * - If the first entry in the oplog is a replica set initialization, then we can resume even if the
 *   token timestamp is earlier, since no events can have fallen off this oplog yet. This can happen
 *   in a sharded cluster when a new shard is added.
 *
 * - Otherwise we cannot resume, as we do not know if there were any events between the resume token
 *   and the first matching document in the oplog.
 */
class DocumentSourceCheckResumability : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalCheckResumability"_sd;

    // Used to record the results of comparing the token data extracted from documents in the
    // resumed stream against the client's resume token.
    enum class ResumeStatus {
        kFoundToken,      // The stream produced a document satisfying the client resume token.
        kSurpassedToken,  // The stream's latest document is more recent than the resume token.
        kCheckNextDoc     // The next document produced by the stream may contain the resume token.
    };

    GetNextResult getNext() override;
    const char* getSourceName() const override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const override {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kAnyShard,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                LookupRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    static boost::intrusive_ptr<DocumentSourceCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Timestamp ts);

    static boost::intrusive_ptr<DocumentSourceCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token);

protected:
    /**
     * Use the create static method to create a DocumentSourceCheckResumability.
     */
    DocumentSourceCheckResumability(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    ResumeTokenData token);

    ResumeStatus _resumeStatus = ResumeStatus::kCheckNextDoc;
    const ResumeTokenData _tokenFromClient;
};

/**
 * This stage is used internally for change streams to ensure that the resume token is in the
 * stream.  It is not intended to be created by the user.
 */
class DocumentSourceEnsureResumeTokenPresent final : public DocumentSourceCheckResumability {
public:
    static constexpr StringData kStageName = "$_internalEnsureResumeTokenPresent"_sd;

    GetNextResult getNext() override;
    const char* getSourceName() const final;

    StageConstraints constraints(Pipeline::SplitState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                // If this is parsed on mongos it should stay on mongos. If we're not in a sharded
                // cluster then it's okay to run on mongod.
                HostTypeRequirement::kLocalOnly,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                LookupRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        DistributedPlanLogic logic;
        // This stage must run on mongos to ensure it sees the resume token, which could have come
        // from any shard.  We also must include a mergingPresorted $sort stage to communicate to
        // the AsyncResultsMerger that we need to merge the streams in a particular order.
        logic.mergingStage = this;
        // Also add logic to the shards to ensure that each shard has enough oplog history to resume
        // the change stream.
        logic.shardsStage = DocumentSourceCheckResumability::create(pExpCtx, _tokenFromClient);
        logic.inputSortPattern = change_stream_constants::kSortSpec;
        return logic;
    };

    static boost::intrusive_ptr<DocumentSourceEnsureResumeTokenPresent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token);

private:
    /**
     * Use the create static method to create a DocumentSourceEnsureResumeTokenPresent.
     */
    DocumentSourceEnsureResumeTokenPresent(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           ResumeTokenData token);

    // Records whether we have observed the token in the resumed stream.
    bool _hasSeenResumeToken = false;
};

}  // namespace mongo
