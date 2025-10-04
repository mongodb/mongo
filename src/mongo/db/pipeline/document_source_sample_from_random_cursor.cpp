/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/pipeline/document_source_sample_from_random_cursor.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <string>
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

const char* DocumentSourceSampleFromRandomCursor::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceSampleFromRandomCursor::serialize(const SerializationOptions& opts) const {
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
