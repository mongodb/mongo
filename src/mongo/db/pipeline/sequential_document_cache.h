// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Implements a sequential cache of Documents, up to an optional maximum size. Can be in one of
 * three states: building, serving, or abandoned. See SequentialDocumentCache::CacheStatus.
 */
class SequentialDocumentCache {
    SequentialDocumentCache(const SequentialDocumentCache&) = delete;
    SequentialDocumentCache& operator=(const SequentialDocumentCache&) = delete;

public:
    explicit SequentialDocumentCache(size_t maxCacheSizeBytes) : _maxSizeBytes(maxCacheSizeBytes) {}

    SequentialDocumentCache(SequentialDocumentCache&& moveFrom)
        : _status(moveFrom._status),
          _maxSizeBytes(moveFrom._maxSizeBytes),
          _sizeBytes(moveFrom._sizeBytes),
          _cacheIter(std::move(moveFrom._cacheIter)),
          _cache(std::move(moveFrom._cache)) {}

    SequentialDocumentCache& operator=(SequentialDocumentCache&& moveFrom) {
        _cacheIter = std::move(moveFrom._cacheIter);
        _maxSizeBytes = moveFrom._maxSizeBytes;
        _cache = std::move(moveFrom._cache);
        _sizeBytes = moveFrom._sizeBytes;
        _status = moveFrom._status;

        return *this;
    }

    /**
     * Defines the states that the cache may be in at any given time.
     */
    enum class CacheStatus {
        // The cache is being filled. More documents may be added. A newly instantiated cache is in
        // this state by default.
        kBuilding,

        // The caller has invoked freeze() to indicate that no more Documents need to be added. The
        // cache is read-only at this point.
        kServing,

        // The maximum permitted cache size has been exceeded, or the caller has explicitly
        // abandoned the cache. Cannot add more documents or call getNext.
        kAbandoned,
    };

    /**
     * Adds a document to the back of the cache. May only be called while the cache is in
     * 'kBuilding' mode.
     */
    void add(Document doc);

    /**
     * Moves the cache into 'kServing' (read-only) mode, and attempts to release any excess
     * allocated memory. May only be called while the cache is in 'kBuilding' mode.
     */
    void freeze();

    /**
     * Abandons the cache, marking it as 'kAbandoned' and freeing any memory allocated while
     * building.
     */
    void abandon();

    /**
     * Returns the next Document in sequence from the cache, or boost::none if the end of the cache
     * has been reached. May only be called while in 'kServing' mode.
     */
    boost::optional<Document> getNext();

    /**
     * Resets the cache iterator to the beginning of the cache. May only be called while the cache
     * is in 'kServing' mode.
     */
    void restartIteration();

    CacheStatus status() const {
        return _status;
    }

    size_t sizeBytes() const {
        return _sizeBytes;
    }

    size_t maxSizeBytes() const {
        return _maxSizeBytes;
    }

    size_t count() const {
        return _cache.size();
    }

    bool isBuilding() const {
        return _status == CacheStatus::kBuilding;
    }

    bool isServing() const {
        return _status == CacheStatus::kServing;
    }

    bool isAbandoned() const {
        return _status == CacheStatus::kAbandoned;
    }


    /**
     * Maps 'var' to 'val' and overwrites any previously existing value for the variable.
     */
    void setCachedVariableValue(Variables::Id var, Value val) {
        _cachedVariables.insert_or_assign(var, val);
    }

    /**
     * Returns value of variable or Missing value if not found.
     */
    Value getCachedVariableValue(Variables::Id var) {
        if (auto val = _cachedVariables.find(var); val != _cachedVariables.end()) {
            return val->second;
        }
        return Value();
    }

private:
    CacheStatus checkCacheSize(const Document& doc);

    CacheStatus _status = CacheStatus::kBuilding;
    size_t _maxSizeBytes = 0;
    size_t _sizeBytes = 0;

    std::vector<Document>::iterator _cacheIter;
    std::vector<Document> _cache;

    absl::flat_hash_map<Variables::Id, Value> _cachedVariables;
};

}  // namespace mongo
