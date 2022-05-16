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

#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {
/**
 * A custom subclass of DocumentSourceMatch which is used to generate a $match stage to be applied
 * on the oplog. The stage requires itself to be the first stage in the pipeline.
 */
class DocumentSourceChangeStreamOplogMatch final : public DocumentSourceMatch {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamOplogMatch"_sd;

    DocumentSourceChangeStreamOplogMatch(Timestamp clusterTime,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceChangeStreamOplogMatch(const DocumentSourceChangeStreamOplogMatch& other,
                                         const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
        : DocumentSourceMatch(other, newExpCtx) {
        _clusterTime = other._clusterTime;
        _optimizedEndOfPipeline = other._optimizedEndOfPipeline;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx = nullptr) const final {
        return new DocumentSourceChangeStreamOplogMatch(*this, newExpCtx);
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamOplogMatch> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    const char* getSourceName() const final;

    GetNextResult doGetNext() final {
        // We should never execute this stage directly. We expect this stage to be absorbed into the
        // cursor feeding the pipeline, and executing this stage may result in the use of the wrong
        // collation. The comparisons against the oplog must use the simple collation, regardless of
        // the collation on the ExpressionContext.
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final;

protected:
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    /**
     * This constructor is only used for deserializing from BSON, in which case there is no value
     * for the '_clusterTime' field. We leave this field as boost::none and assume that it will not
     * be needed. We also assume that optimizations have have already been applied.
     */
    DocumentSourceChangeStreamOplogMatch(BSONObj filter,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMatch(std::move(filter), expCtx), _optimizedEndOfPipeline(true) {
        expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;
    }

    // Needed for re-creating the filter during optimization. Note that we do not serialize these
    // fields. The filter in a serialized DocumentSourceOplogMatch is considered final, so there is
    // no need to re-create it.
    boost::optional<Timestamp> _clusterTime;

    // Used to avoid infinte optimization loops. Note that we do not serialize this field, because
    // we assume that DocumentSourceOplogMatch is always serialized after optimization.
    bool _optimizedEndOfPipeline = false;
};
}  // namespace mongo
