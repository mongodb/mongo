// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_tee_consumer.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

ALLOCATE_DOCUMENT_SOURCE_ID(teeConsumer, DocumentSourceTeeConsumer::id)

DocumentSourceTeeConsumer::DocumentSourceTeeConsumer(const intrusive_ptr<ExpressionContext>& expCtx,
                                                     size_t facetId,
                                                     std::string_view stageName)
    : DocumentSource(stageName, expCtx), _facetId(facetId), _stageName(std::string{stageName}) {}

boost::intrusive_ptr<DocumentSourceTeeConsumer> DocumentSourceTeeConsumer::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    size_t facetId,
    std::string_view stageName) {
    return new DocumentSourceTeeConsumer(expCtx, facetId, stageName);
}

std::string_view DocumentSourceTeeConsumer::getSourceName() const {
    return _stageName;
}

Value DocumentSourceTeeConsumer::serialize(const query_shape::SerializationOptions& opts) const {
    // We only serialize this stage in the context of explain.
    return opts.isSerializingForExplain() ? Value(DOC(_stageName << Document())) : Value();
}
}  // namespace mongo
