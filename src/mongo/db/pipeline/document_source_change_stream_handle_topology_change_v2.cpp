// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamHandleTopologyChangeV2,
                            DocumentSourceChangeStreamHandleTopologyChangeV2::id)

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2>
DocumentSourceChangeStreamHandleTopologyChangeV2::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(10612600,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return create(expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamHandleTopologyChangeV2>
DocumentSourceChangeStreamHandleTopologyChangeV2::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceChangeStreamHandleTopologyChangeV2(expCtx);
}

DocumentSourceChangeStreamHandleTopologyChangeV2::DocumentSourceChangeStreamHandleTopologyChangeV2(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {}

StageConstraints DocumentSourceChangeStreamHandleTopologyChangeV2::constraints(
    PipelineSplitState pipeState) const {
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

Value DocumentSourceChangeStreamHandleTopologyChangeV2::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForQueryStats()) {
        // Stages made internally by 'DocumentSourceChangeStream' should not be serialized for
        // query stats. For query stats we will serialize only the user specified $changeStream
        // stage.
        return Value();
    }
    if (opts.isSerializingForExplain()) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage" << "internalHandleTopologyChangeV2"sv)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
