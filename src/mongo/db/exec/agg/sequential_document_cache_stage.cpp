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

#include "mongo/db/exec/agg/sequential_document_cache_stage.h"

#include "mongo/base/init.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/optional/optional.hpp>

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
    StringData stageName,
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
