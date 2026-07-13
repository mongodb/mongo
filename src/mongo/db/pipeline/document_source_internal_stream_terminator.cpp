// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_stream_terminator.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalStreamTerminator,
                                              InternalStreamTerminatorLiteParsed::parse);

ALLOCATE_STAGE_PARAMS_ID(_internalStreamTerminator, InternalStreamTerminatorStageParams::id);

DocumentSourceContainer internalStreamTerminatorStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return {make_intrusive<DocumentSourceInternalStreamTerminator>(expCtx)};
}

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(
    _internalStreamTerminator,
    InternalStreamTerminatorStageParams::id,
    internalStreamTerminatorStageParamsToDocumentSourceFn);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalStreamTerminator, DocumentSourceInternalStreamTerminator::id);

constexpr std::string_view DocumentSourceInternalStreamTerminator::kStageName;

Value DocumentSourceInternalStreamTerminator::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), Value{Document{}}}});
}

}  // namespace mongo
