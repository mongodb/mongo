// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_convert_errors.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamConvertErrors,
                                              ChangeStreamConvertErrorsLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamConvertErrors,
                                                   DocumentSourceChangeStreamConvertErrors,
                                                   ChangeStreamConvertErrorsStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamConvertErrors,
                            DocumentSourceChangeStreamConvertErrors::id)

boost::intrusive_ptr<DocumentSourceChangeStreamConvertErrors>
DocumentSourceChangeStreamConvertErrors::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamConvertErrors(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamConvertErrors>
DocumentSourceChangeStreamConvertErrors::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(13731904,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            spec.type() == BSONType::object && spec.Obj().isEmpty());
    return new DocumentSourceChangeStreamConvertErrors(expCtx);
}

std::string_view DocumentSourceChangeStreamConvertErrors::getSourceName() const {
    return kStageName;
}

StageConstraints DocumentSourceChangeStreamConvertErrors::constraints(PipelineSplitState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 // Must be the final stage (the top of the pipeline, above every
                                 // user stage): an error event is built there and must never be
                                 // observed by a user stage below it.
                                 PositionRequirement::kLast,
                                 // Runs on whichever node executes the pipeline: the router, or the
                                 // data-bearing node (standalone, replica set, or shard). Injected
                                 // post-optimization, so this only matters when a serialized shard
                                 // pipeline is re-parsed and re-optimized on the shard.
                                 HostTypeRequirement::kReceivingHostOnly,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage};
    // It must stay above every user stage and must not be swapped past by them: an error event is
    // built above the user stages and must never be observed by them.
    constraints.canSwapWithMatch = false;
    constraints.canSwapWithSingleDocTransformOrRedact = false;
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;
    constraints.canAppearOnlyOnceInPipeline = true;
    return constraints;
}

Value DocumentSourceChangeStreamConvertErrors::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.isSerializingForExplain()) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"sv, kStageName);
        sub.done();
    } else {
        builder.append(kStageName, BSONObj());
    }
    return Value(builder.obj());
}

}  // namespace mongo
