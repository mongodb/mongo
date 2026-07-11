// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalChangeStreamCheckTopologyChange,
                                              ChangeStreamCheckTopologyChangeLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(_internalChangeStreamCheckTopologyChange,
                                                   DocumentSourceChangeStreamCheckTopologyChange,
                                                   ChangeStreamCheckTopologyChangeStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamCheckTopologyChange,
                            DocumentSourceChangeStreamCheckTopologyChange::id)

StageConstraints DocumentSourceChangeStreamCheckTopologyChange::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kTargetedShards,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;
    return constraints;
}

boost::intrusive_ptr<DocumentSourceChangeStreamCheckTopologyChange>
DocumentSourceChangeStreamCheckTopologyChange::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5669601,
            str::stream() << "the '" << kStageName << "' spec must be an object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());
    return new DocumentSourceChangeStreamCheckTopologyChange(expCtx);
}

Value DocumentSourceChangeStreamCheckTopologyChange::doSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(DOC(DocumentSourceChangeStream::kStageName
                         << DOC("stage" << "internalCheckTopologyChange"sv)));
    }

    return Value(Document{{kStageName, Document()}});
}

}  // namespace mongo
