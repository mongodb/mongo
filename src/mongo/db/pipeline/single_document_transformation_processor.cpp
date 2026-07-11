// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/single_document_transformation_processor.h"

#include "mongo/db/exec/document_value/document.h"


namespace mongo {


SingleDocumentTransformationProcessor::SingleDocumentTransformationProcessor(
    std::unique_ptr<TransformerInterface> parsedTransform)
    : _parsedTransform(std::move(parsedTransform)) {}

Document SingleDocumentTransformationProcessor::process(const Document& input,
                                                        const EvaluationContext& ctx) const {
    dassert(_parsedTransform);
    // Apply and return the document with added fields.
    return _parsedTransform->applyTransformation(input, ctx);
}

}  // namespace mongo
