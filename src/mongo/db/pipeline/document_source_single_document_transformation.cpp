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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_single_document_transformation.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceSingleDocumentTransformation::DocumentSourceSingleDocumentTransformation(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<TransformerInterface> parsedTransform,
    const StringData name,
    bool isIndependentOfAnyCollection)
    : DocumentSource(name, pExpCtx),
      _parsedTransform(std::move(parsedTransform)),
      _name(name.toString()),
      _isIndependentOfAnyCollection(isIndependentOfAnyCollection) {}

const char* DocumentSourceSingleDocumentTransformation::getSourceName() const {
    return _name.c_str();
}

DocumentSource::GetNextResult DocumentSourceSingleDocumentTransformation::doGetNext() {
    if (!_parsedTransform) {
        return DocumentSource::GetNextResult::makeEOF();
    }

    // Get the next input document.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    // Apply and return the document with added fields.
    return _parsedTransform->applyTransformation(input.releaseDocument());
}

intrusive_ptr<DocumentSource> DocumentSourceSingleDocumentTransformation::optimize() {
    if (_parsedTransform) {
        _parsedTransform->optimize();
    }
    return this;
}

void DocumentSourceSingleDocumentTransformation::doDispose() {
    if (_parsedTransform) {
        // Cache the stage options document in case this stage is serialized after disposing.
        _cachedStageOptions = _parsedTransform->serializeTransformation(pExpCtx->explain);
        _parsedTransform.reset();
    }
}

Value DocumentSourceSingleDocumentTransformation::serialize(SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484312);
    }

    return Value(
        Document{{getSourceName(),
                  _parsedTransform ? _parsedTransform->serializeTransformation(opts.verbosity)
                                   : _cachedStageOptions}});
}

Pipeline::SourceContainer::iterator DocumentSourceSingleDocumentTransformation::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextSkip = dynamic_cast<DocumentSourceSkip*>((*std::next(itr)).get());

    if (nextSkip) {
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

DepsTracker::State DocumentSourceSingleDocumentTransformation::getDependencies(
    DepsTracker* deps) const {
    // Each parsed transformation is responsible for adding its own dependencies, and returning
    // the correct dependency return type for that transformation.
    return _parsedTransform->addDependencies(deps);
}

void DocumentSourceSingleDocumentTransformation::addVariableRefs(
    std::set<Variables::Id>* refs) const {
    _parsedTransform->addVariableRefs(refs);
}

DocumentSource::GetModPathsReturn DocumentSourceSingleDocumentTransformation::getModifiedPaths()
    const {
    return _parsedTransform->getModifiedPaths();
}

}  // namespace mongo
