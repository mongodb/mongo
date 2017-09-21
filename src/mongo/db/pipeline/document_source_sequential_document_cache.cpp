/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_sequential_document_cache.h"

#include "mongo/db/pipeline/document_source_match.h"

namespace mongo {

constexpr StringData DocumentSourceSequentialDocumentCache::kStageName;

DocumentSourceSequentialDocumentCache::DocumentSourceSequentialDocumentCache(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, SequentialDocumentCache* cache)
    : DocumentSource(expCtx), _cache(cache) {
    invariant(_cache);
    invariant(!_cache->isAbandoned());

    if (_cache->isServing()) {
        _cache->restartIteration();
    }
}

DocumentSource::GetNextResult DocumentSourceSequentialDocumentCache::getNext() {
    // Either we're reading from the cache, or we have an input source to build the cache from.
    invariant(pSource || _cache->isServing());

    pExpCtx->checkForInterrupt();

    if (_cache->isServing()) {
        auto nextDoc = _cache->getNext();
        return (nextDoc ? std::move(*nextDoc) : GetNextResult::makeEOF());
    }

    auto nextResult = pSource->getNext();

    if (!_cache->isAbandoned()) {
        if (nextResult.isEOF()) {
            _cache->freeze();
        } else {
            _cache->add(nextResult.getDocument());
        }
    }

    return nextResult;
}

Pipeline::SourceContainer::iterator DocumentSourceSequentialDocumentCache::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    // The DocumentSourceSequentialDocumentCache should always be the last stage in the pipeline
    // pre-optimization. By the time optimization reaches this point, all preceding stages are in
    // the final positions which they would have occupied if no cache stage was present.
    invariant(_hasOptimizedPos || std::next(itr) == container->end());
    invariant((*itr).get() == this);

    // If we have already optimized our position, stay where we are.
    if (_hasOptimizedPos) {
        return std::next(itr);
    }

    // Mark this stage as having optimized itself.
    _hasOptimizedPos = true;

    // If the cache is the only stage in the pipeline, return immediately.
    if (itr == container->begin()) {
        return container->end();
    }

    // Pop the cache stage off the back of the pipeline.
    auto cacheStage = std::move(*itr);
    container->erase(itr);

    // Get all variable IDs defined in this scope.
    auto varIDs = pExpCtx->variablesParseState.getDefinedVariableIDs();

    auto prefixSplit = container->begin();
    DepsTracker deps;

    // Iterate through the pipeline stages until we find one which references an external variable.
    for (; prefixSplit != container->end(); ++prefixSplit) {
        (*prefixSplit)->getDependencies(&deps);

        if (deps.hasVariableReferenceTo(varIDs)) {
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

Value DocumentSourceSequentialDocumentCache::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{
            {kStageName,
             Document{{"maxSizeBytes"_sd, Value(static_cast<long long>(_cache->maxSizeBytes()))},
                      {"status"_sd,
                       _cache->isBuilding() ? "kBuilding"_sd : _cache->isServing()
                               ? "kServing"_sd
                               : "kAbandoned"_sd}}}});
    }

    return Value();
}

}  // namesace mongo
