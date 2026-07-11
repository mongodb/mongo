// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_sequential_document_cache.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <list>
#include <set>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

ALLOCATE_DOCUMENT_SOURCE_ID(sequentialCache, DocumentSourceSequentialDocumentCache::id)

constexpr std::string_view DocumentSourceSequentialDocumentCache::kStageName;

DocumentSourceSequentialDocumentCache::DocumentSourceSequentialDocumentCache(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, SequentialDocumentCachePtr cache)
    : DocumentSource(kStageName, expCtx), _cache(std::move(cache)) {
    tassert(11282970, "Missing cache", _cache);

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


DocumentSourceContainer::iterator DocumentSourceSequentialDocumentCache::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // The DocumentSourceSequentialDocumentCache relies on all other stages in the pipeline being at
    // the final positions which they would have occupied if no cache stage was present. This should
    // be the case when we reach this function. The cache should always be the last stage in the
    // pipeline pre-optimizing.
    tassert(11282969,
            "Expecting cache stage to always be the last stage in the pipeline pre-optimizing",
            _hasOptimizedPos || std::next(itr) == container->end());
    tassert(11282968, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

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
    //     $search and $vectorSearch are exceptions to rule 1, as they don't depend on other
    //     stages; their score metadata travels inside the cached Documents.
    //  2. depends on a variable defined in this scope, or
    //  3. generates random numbers.
    std::set<Variables::Id> prefixVarRefs;
    for (; prefixSplit != container->end(); ++prefixSplit) {
        (*prefixSplit)->addVariableRefs(&prefixVarRefs);

        bool isNotSearch = !(*prefixSplit)->isInstanceOf<DocumentSourceSearch>() &&
            !(*prefixSplit)->isInstanceOf<DocumentSourceVectorSearch>();
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
    // attempt to perform any caching. Abandon the cache and return. If the cache is already serving
    // documents, it should not be abandoned, because it must be the case that the the documents are
    // uncorrelated.
    if (prefixSplit == container->begin() && !_cache->isServing()) {
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

Value DocumentSourceSequentialDocumentCache::serialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForExplain()) {
        return Value(Document{
            {kStageName,
             Document{{"maxSizeBytes"sv,
                       opts.serializeLiteral(static_cast<long long>(_cache->maxSizeBytes()))},
                      {"status"sv,
                       _cache->isBuilding()      ? "kBuilding"sv
                           : _cache->isServing() ? "kServing"sv
                                                 : "kAbandoned"sv}}}});
    }

    return Value();
}

}  // namespace mongo
