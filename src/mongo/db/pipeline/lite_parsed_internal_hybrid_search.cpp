/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/lite_parsed_internal_hybrid_search.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(internalHybridSearch, InternalHybridSearchStageParams::id);

namespace {

// Build the real (passthrough) $_internalHybridSearch DocumentSource so the marker survives
// serialization to shards; its LiteParsed carries canRunOnTimeseries=false, enforced at each
// collection acquisition via validateWithCollectionMetadata.
DocumentSourceContainer internalHybridSearchToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<InternalHybridSearchStageParams*>(stageParams.get());
    tassert(12109102, "expected 'InternalHybridSearchStageParams' type", typedParams);
    return {
        DocumentSourceInternalHybridSearch::createFromBson(typedParams->getOriginalBson(), expCtx)};
}

}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(internalHybridSearch,
                                                 InternalHybridSearchStageParams::id,
                                                 internalHybridSearchToDocumentSourceFn);

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalHybridSearch,
                                              LiteParsedInternalHybridSearch::parse);

}  // namespace mongo
