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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
class DocumentSourceChangeStreamCheckResumability : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamCheckResumability"_sd;

    const char* getSourceName() const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckResumability> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckResumability> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static const Id& id;

    Id getId() const override {
        return id;
    }

protected:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamCheckResumabilityToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamCheckResumability.
     */
    DocumentSourceChangeStreamCheckResumability(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, ResumeTokenData token);

    const ResumeTokenData _tokenFromClient;
};
}  // namespace mongo
