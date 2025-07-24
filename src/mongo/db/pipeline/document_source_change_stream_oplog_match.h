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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/tailable_mode_gen.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * A custom subclass of DocumentSourceMatch which is used to generate a $match stage to be applied
 * on the oplog. The stage requires itself to be the first stage in the pipeline.
 */
class DocumentSourceChangeStreamOplogMatch final : public DocumentSourceInternalChangeStreamMatch {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamOplogMatch"_sd;

    DocumentSourceChangeStreamOplogMatch(Timestamp clusterTime,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         std::unique_ptr<MatchExpression> opLogMatchFilter,
                                         std::vector<BSONObj> backingBsonObjs);

    DocumentSourceChangeStreamOplogMatch(const DocumentSourceChangeStreamOplogMatch& other,
                                         const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
        : DocumentSourceInternalChangeStreamMatch(other, newExpCtx) {
        _clusterTime = other._clusterTime;
        _optimizedEndOfPipeline = other._optimizedEndOfPipeline;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const final {
        return new DocumentSourceChangeStreamOplogMatch(*this, newExpCtx);
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamOplogMatch> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    const char* getSourceName() const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    Value doSerialize(const SerializationOptions& opts) const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    /**
     * This constructor is only used for deserializing from BSON, in which case there is no value
     * for the '_clusterTime' field. We leave this field as boost::none and assume that it will not
     * be needed. We also assume that optimizations have have already been applied.
     */
    DocumentSourceChangeStreamOplogMatch(BSONObj filter,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceInternalChangeStreamMatch(filter, expCtx), _optimizedEndOfPipeline(true) {
        expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);
    }

    // Needed for re-creating the filter during optimization. Note that we do not serialize these
    // fields. The filter in a serialized DocumentSourceOplogMatch is considered final, so there is
    // no need to re-create it.
    boost::optional<Timestamp> _clusterTime;

    // Stores the BSONObj used in building the OplogMatch MatchExpression. The BSONObj need to be
    // kept for the query runtime.
    std::vector<BSONObj> _backingBsonObjs;

    // Used to avoid infinite optimization loops. Note that we do not serialize this field, because
    // we assume that DocumentSourceOplogMatch is always serialized after optimization.
    bool _optimizedEndOfPipeline = false;
};
}  // namespace mongo
