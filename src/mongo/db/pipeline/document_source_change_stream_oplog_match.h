// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ChangeStreamOplogMatch);
using ChangeStreamOplogMatchLiteParsed =
    DocumentSourceChangeStreamLiteParsedInternal<ChangeStreamOplogMatchStageParams>;

/**
 * A custom subclass of DocumentSourceMatch which is used to generate a $match stage to be applied
 * on the oplog. The stage requires itself to be the first stage in the pipeline.
 */
class DocumentSourceChangeStreamOplogMatch final : public DocumentSourceInternalChangeStreamMatch {
public:
    static constexpr std::string_view kStageName = "$_internalChangeStreamOplogMatch"sv;

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

    std::string_view getSourceName() const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    Value doSerialize(const query_shape::SerializationOptions& opts) const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    DocumentSourceContainer::iterator optimizeAt(DocumentSourceContainer::iterator itr,
                                                 DocumentSourceContainer* container);

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
