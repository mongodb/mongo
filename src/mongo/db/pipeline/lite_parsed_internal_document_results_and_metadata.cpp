// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/str.h"

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(_internalDocumentResultsAndMetadata,
                         InternalDocumentResultsAndMetadataStageParams::id);

std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed>
InternalDocumentResultsAndMetadataLiteParsed::parse(const NamespaceString& nss,
                                                    const BSONElement& spec,
                                                    const LiteParserOptions& options) {
    tassert(ErrorCodes::FailedToParse,
            str::stream() << "$_internalDocumentResultsAndMetadata specification must be an "
                             "object, found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    auto parsedSpec = DocumentSourceResultsAndMetadataSpec::parse(
        spec.embeddedObject(), IDLParserContext("$_internalDocumentResultsAndMetadata"));

    auto sourceElem = spec.embeddedObject()["source"];
    tassert(ErrorCodes::FailedToParse,
            "$_internalDocumentResultsAndMetadata requires a 'source' field",
            !sourceElem.eoo());
    tassert(ErrorCodes::FailedToParse,
            "$_internalDocumentResultsAndMetadata 'source' must be an object",
            sourceElem.type() == BSONType::object);

    OwnedLiteParsedPipeline sourcePipeline(nss, {sourceElem.embeddedObject()}, options);
    LiteParsedDesugarer::desugar(&*sourcePipeline, options.ifrContext);

    auto liteParsed = std::make_unique<InternalDocumentResultsAndMetadataLiteParsed>(
        spec, parsedSpec.getMetadata(), parsedSpec.getReturnCursor(), std::move(sourcePipeline));

    if (const auto& planSpec = parsedSpec.getShardedPlanSpec()) {
        liteParsed->setShardedPlan(*planSpec);
    }

    return liteParsed;
}

}  // namespace mongo
