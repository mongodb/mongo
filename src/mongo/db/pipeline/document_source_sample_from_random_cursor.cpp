// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
using boost::intrusive_ptr;

ALLOCATE_DOCUMENT_SOURCE_ID(sampleFromRandomCursor, DocumentSourceSampleFromRandomCursor::id)

DocumentSourceSampleFromRandomCursor::DocumentSourceSampleFromRandomCursor(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    long long size,
    std::string idField,
    long long nDocsInCollection)
    : DocumentSource(kStageName, pExpCtx),
      _size(size),
      _idField(std::move(idField)),
      _nDocsInColl(nDocsInCollection) {}

std::string_view DocumentSourceSampleFromRandomCursor::getSourceName() const {
    return kStageName;
}

Value DocumentSourceSampleFromRandomCursor::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << DOC("size" << opts.serializeLiteral(_size))));
}

DepsTracker::State DocumentSourceSampleFromRandomCursor::getDependencies(DepsTracker* deps) const {
    deps->fields.insert(_idField);
    return DepsTracker::State::SEE_NEXT;
}

intrusive_ptr<DocumentSourceSampleFromRandomCursor> DocumentSourceSampleFromRandomCursor::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    long long size,
    std::string idField,
    long long nDocsInCollection) {
    intrusive_ptr<DocumentSourceSampleFromRandomCursor> source(
        new DocumentSourceSampleFromRandomCursor(expCtx, size, idField, nDocsInCollection));
    return source;
}
}  // namespace mongo
