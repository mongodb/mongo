// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/sequential_document_cache_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>


namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSequentialDocumentCacheToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* sequentialDocumentCacheDS =
        dynamic_cast<DocumentSourceSequentialDocumentCache*>(documentSource.get());
    tassert(10886000,
            "expected 'DocumentSourceSequentialDocumentCache' type",
            sequentialDocumentCacheDS);
    return make_intrusive<exec::agg::SequentialDocumentCacheStage>(
        DocumentSourceSequentialDocumentCache::kStageName,
        documentSource->getExpCtx(),
        sequentialDocumentCacheDS->_cache);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(SequentialDocumentCacheStage,
                           DocumentSourceSequentialDocumentCache::id,
                           documentSourceSequentialDocumentCacheToStageFn);

SequentialDocumentCacheStage::SequentialDocumentCacheStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    SequentialDocumentCachePtr cache)
    : Stage(stageName, pExpCtx), _cache(std::move(cache)) {}

GetNextResult SequentialDocumentCacheStage::doGetNext() {
    // Either we're reading from the cache, or we have an input source to build the cache from.
    invariant(pSource || _cache->isServing());

    if (_cacheIsEOF) {
        return GetNextResult::makeEOF();
    }

    if (_cache->isServing()) {
        auto nextDoc = _cache->getNext();
        if (nextDoc) {
            return std::move(*nextDoc);
        }
        _cacheIsEOF = true;
        return GetNextResult::makeEOF();
    }

    auto nextResult = pSource->getNext();

    if (!_cache->isAbandoned()) {
        if (nextResult.isEOF()) {
            _cache->freeze();
            _cacheIsEOF = true;

            // SearchMeta may be set in the expCtx, and it should be persisted through the cache. If
            // not persisted, SearchMeta will be missing when it may be needed for future executions
            // of the pipeline.
            if (auto searchMetaVal = pExpCtx->variables.getValue(Variables::kSearchMetaId);
                !searchMetaVal.missing()) {
                _cache->setCachedVariableValue(Variables::kSearchMetaId, searchMetaVal);
            }
        } else {
            _cache->add(nextResult.getDocument());
        }
    }

    return nextResult;
}
}  // namespace exec::agg
}  // namespace mongo
