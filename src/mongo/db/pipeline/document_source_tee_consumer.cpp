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

#include "mongo/db/pipeline/document_source_tee_consumer.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceTeeConsumer::DocumentSourceTeeConsumer(const intrusive_ptr<ExpressionContext>& expCtx,
                                                     size_t facetId,
                                                     const intrusive_ptr<TeeBuffer>& bufferSource,
                                                     StringData stageName)
    : DocumentSource(stageName, expCtx),
      _facetId(facetId),
      _bufferSource(bufferSource),
      _stageName(stageName.toString()) {}

boost::intrusive_ptr<DocumentSourceTeeConsumer> DocumentSourceTeeConsumer::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    size_t facetId,
    const boost::intrusive_ptr<TeeBuffer>& bufferSource,
    StringData stageName) {
    return new DocumentSourceTeeConsumer(expCtx, facetId, bufferSource, stageName);
}

const char* DocumentSourceTeeConsumer::getSourceName() const {
    return _stageName.c_str();
}

DocumentSource::GetNextResult DocumentSourceTeeConsumer::doGetNext() {
    return _bufferSource->getNext(_facetId);
}

void DocumentSourceTeeConsumer::doDispose() {
    _bufferSource->dispose(_facetId);
}

Value DocumentSourceTeeConsumer::serialize(const SerializationOptions& opts) const {
    // We only serialize this stage in the context of explain.
    return opts.verbosity ? Value(DOC(_stageName << Document())) : Value();
}
}  // namespace mongo
