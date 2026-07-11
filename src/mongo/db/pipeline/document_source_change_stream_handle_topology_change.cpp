// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamHandleTopologyChange,
                                              ChangeStreamHandleTopologyChangeLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamHandleTopologyChange,
                                                   DocumentSourceChangeStreamHandleTopologyChange,
                                                   ChangeStreamHandleTopologyChangeStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamHandleTopologyChange,
                            DocumentSourceChangeStreamHandleTopologyChange::id)

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange>
DocumentSourceChangeStreamHandleTopologyChange::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(8131300,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return new DocumentSourceChangeStreamHandleTopologyChange(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChange>
DocumentSourceChangeStreamHandleTopologyChange::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamHandleTopologyChange(expCtx);
}

DocumentSourceChangeStreamHandleTopologyChange::DocumentSourceChangeStreamHandleTopologyChange(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}

StageConstraints DocumentSourceChangeStreamHandleTopologyChange::constraints(
    PipelineSplitState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kRouter,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage};

    // Can be swapped with the '$match', '$redact', and 'DocumentSourceSingleDocumentTransformation'
    // stages and ensures that they get pushed down to the shards, as this stage bisects the change
    // streams pipeline.
    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSingleDocTransformOrRedact = true;
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;

    return constraints;
}

Value DocumentSourceChangeStreamHandleTopologyChange::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage" << "internalHandleTopologyChange"sv)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
