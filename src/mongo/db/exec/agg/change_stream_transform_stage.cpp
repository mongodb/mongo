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

#include "mongo/db/exec/agg/change_stream_transform_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/util/assert_util.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamTransformToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamTransformDS =
        dynamic_cast<DocumentSourceChangeStreamTransform*>(documentSource.get());

    tassert(
        10561306, "expected 'DocumentSourceChangeStreamTransform' type", changeStreamTransformDS);

    return make_intrusive<exec::agg::ChangeStreamTransformStage>(
        changeStreamTransformDS->kStageName,
        changeStreamTransformDS->getExpCtx(),
        changeStreamTransformDS->_transformer);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamTransform,
                           DocumentSourceChangeStreamTransform::id,
                           documentSourceChangeStreamTransformToStageFn)

ChangeStreamTransformStage::ChangeStreamTransformStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::shared_ptr<ChangeStreamEventTransformer> transformer)
    : Stage(stageName, pExpCtx), _transformer(std::move(transformer)) {
    uassert(50988,
            "Illegal attempt to execute an internal change stream stage on router. A $changeStream "
            "stage must be the first stage in a pipeline",
            !pExpCtx->getInRouter());
}

GetNextResult ChangeStreamTransformStage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    return _transformer->applyTransformation(input.releaseDocument());
}

}  // namespace exec::agg
}  // namespace mongo
