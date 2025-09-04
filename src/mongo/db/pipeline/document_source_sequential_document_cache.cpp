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

#include "mongo/db/pipeline/document_source_sequential_document_cache.h"

#include "mongo/base/init.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <list>
#include <set>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(sequentialCache, DocumentSourceSequentialDocumentCache::id)

constexpr StringData DocumentSourceSequentialDocumentCache::kStageName;

DocumentSourceSequentialDocumentCache::DocumentSourceSequentialDocumentCache(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, SequentialDocumentCachePtr cache)
    : DocumentSource(kStageName, expCtx), _cache(std::move(cache)) {
    invariant(_cache);

    if (_cache->isServing()) {
        _cache->restartIteration();
    }

    // If SearchMeta was stored, it is now set in the expCtx for future stages of the subpipeline.
    if (auto searchMetaVal = _cache->getCachedVariableValue(Variables::kSearchMetaId);
        !searchMetaVal.missing()) {
        tassert(6381601,
                "SEARCH_META variable should not have been set in this expCtx yet.",
                expCtx->variables.getValue(Variables::kSearchMetaId).missing());
        expCtx->variables.setReservedValue(Variables::kSearchMetaId, searchMetaVal, true);
    }
}


DocumentSourceContainer::iterator DocumentSourceSequentialDocumentCache::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // The DocumentSourceSequentialDocumentCache relies on all other stages in the pipeline being at
    // the final positions which they would have occupied if no cache stage was present. This should
    // be the case when we reach this function. The cache should always be the last stage in the
    // pipeline pre-optimizing.
    invariant(_hasOptimizedPos || std::next(itr) == container->end());
    invariant((*itr).get() == this);

    // If we have already optimized our position, stay where we are.
    if (_hasOptimizedPos) {
        return std::next(itr);
    }

    // Mark this stage as having optimized itself.
    _hasOptimizedPos = true;

    // If the cache is the only stage in the pipeline, return immediately.
    if (itr == container->begin() && std::next(itr) == container->end()) {
        return container->end();
    }

    // Pop the cache stage off the back of the pipeline.
    auto cacheStage = std::move(*itr);
    container->erase(itr);

    // Get all variable IDs defined in this scope.
    auto varIDs = getExpCtx()->variablesParseState.getDefinedVariableIDs();

    auto prefixSplit = container->begin();

    DepsTracker deps;

    // Iterate through the pipeline stages until we find one which cannot be cached.
    // A stage cannot be cached if it either:
    //  1. does not support dependency tracking, and may thus require the full object and metadata.
    //     $search is an exception to rule 1, as it doesn't depend on other stages.
    //  2. depends on a variable defined in this scope, or
    //  3. generates random numbers.
    std::set<Variables::Id> prefixVarRefs;
    for (; prefixSplit != container->end(); ++prefixSplit) {
        (*prefixSplit)->addVariableRefs(&prefixVarRefs);

        bool isNotSearch = ((*prefixSplit)->getSourceName() != DocumentSourceSearch::kStageName);
        bool doesNotSupportDependencies =
            ((*prefixSplit)->getDependencies(&deps) == DepsTracker::State::NOT_SUPPORTED);

        if ((isNotSearch && doesNotSupportDependencies) ||
            ((Variables::hasVariableReferenceTo(prefixVarRefs, varIDs) ||
              deps.needRandomGenerator))) {
            break;
        }
    }

    // The 'prefixSplit' iterator is now pointing to the first stage of the correlated suffix. If
    // the split point is the first stage, then the entire pipeline is correlated and we should not
    // attempt to perform any caching. Abandon the cache and return.
    if (prefixSplit == container->begin()) {
        _cache->abandon();
        return container->end();
    }

    // If the cache has been populated and is serving results, remove the non-correlated prefix.
    if (_cache->isServing()) {
        container->erase(container->begin(), prefixSplit);
    }

    container->insert(prefixSplit, std::move(cacheStage));

    return container->end();
}

Value DocumentSourceSequentialDocumentCache::serialize(const SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(Document{
            {kStageName,
             Document{{"maxSizeBytes"_sd,
                       opts.serializeLiteral(static_cast<long long>(_cache->maxSizeBytes()))},
                      {"status"_sd,
                       _cache->isBuilding()      ? "kBuilding"_sd
                           : _cache->isServing() ? "kServing"_sd
                                                 : "kAbandoned"_sd}}}});
    }

    return Value();
}

}  // namespace mongo
