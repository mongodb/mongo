/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/single_document_transformation_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSingleDocumentTransformationToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto singleDocumentTransformationDS =
        boost::dynamic_pointer_cast<DocumentSourceSingleDocumentTransformation>(documentSource);

    tassert(10422800,
            "expected 'DocumentSourceSingleDocumentTransformation' type",
            singleDocumentTransformationDS);

    // Cache the stage options document in case this document source is serialized after
    // disposing this stage.
    singleDocumentTransformationDS->_cachedStageOptions =
        singleDocumentTransformationDS->getTransformer().serializeTransformation(
            SerializationOptions{.verbosity =
                                     singleDocumentTransformationDS->getExpCtx()->getExplain()});

    return make_intrusive<exec::agg::SingleDocumentTransformationStage>(
        singleDocumentTransformationDS->getSourceName(),
        singleDocumentTransformationDS->getExpCtx(),
        singleDocumentTransformationDS->_transformationProcessor);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(singleDocumentTransformation,
                           DocumentSourceSingleDocumentTransformation::id,
                           documentSourceSingleDocumentTransformationToStageFn)

SingleDocumentTransformationStage::SingleDocumentTransformationStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const std::shared_ptr<SingleDocumentTransformationProcessor>& transformationProcessor)
    : Stage(stageName, pExpCtx), _transformationProcessor(transformationProcessor) {}

GetNextResult SingleDocumentTransformationStage::doGetNext() {
    if (!_transformationProcessor) {
        return GetNextResult::makeEOF();
    }

    // Get the next input document.
    auto input = pSource->getNext();

    if (!input.isAdvanced()) {
        return input;
    }

    // Apply and return the document with added fields.
    return _transformationProcessor->process(input.releaseDocument());
}

void SingleDocumentTransformationStage::doDispose() {
    if (_transformationProcessor) {
        _transformationProcessor.reset();
    }
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
