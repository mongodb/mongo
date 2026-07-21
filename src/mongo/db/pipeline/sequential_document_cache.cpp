// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/sequential_document_cache.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void SequentialDocumentCache::add(Document doc) {
    tassert(11282925,
            str::stream() << "Expect SequentialDocumentCache to be in Building ("
                          << int(CacheStatus::kBuilding) << ") state, got " << int(_status),
            _status == CacheStatus::kBuilding);

    if (checkCacheSize(doc) != CacheStatus::kAbandoned) {
        _sizeBytes += doc.getApproximateSize();
        _cache.push_back(std::move(doc));
    }
}

void SequentialDocumentCache::freeze() {
    tassert(11282924,
            str::stream() << "Expect SequentialDocumentCache to be in Building ("
                          << int(CacheStatus::kBuilding) << ") state, got " << int(_status),
            _status == CacheStatus::kBuilding);

    _status = CacheStatus::kServing;
    _cache.shrink_to_fit();

    // Materialize each cached document into owned BSON once, now that the cache is read-only.
    // Downstream consumers that read the same documents repeatedly (e.g. a nested-loop $lookup
    // re-matching the cached prefix for every input document) can then match against the
    // already-BSON-backed document instead of re-serializing a projection on every pass. Documents
    // carrying metadata are left as-is so no metadata is dropped.
    for (auto& doc : _cache) {
        if (!doc.metadata() && !doc.isTriviallyConvertible()) {
            doc = Document(doc.toBson());
        }
    }

    _cacheIter = _cache.begin();
}

void SequentialDocumentCache::abandon() {
    _status = CacheStatus::kAbandoned;

    _cache.clear();
    _cache.shrink_to_fit();

    _cacheIter = _cache.begin();
}

boost::optional<Document> SequentialDocumentCache::getNext() {
    tassert(11282923,
            str::stream() << "Expect SequentialDocumentCache to be in Serving ("
                          << int(CacheStatus::kServing) << ") state, got " << int(_status),
            _status == CacheStatus::kServing);

    if (_cacheIter == _cache.end()) {
        return boost::none;
    }

    return *_cacheIter++;
}

void SequentialDocumentCache::restartIteration() {
    tassert(11282922,
            str::stream() << "Expect SequentialDocumentCache to be in Serving ("
                          << int(CacheStatus::kServing) << ") state, got " << int(_status),
            _status == CacheStatus::kServing);
    _cacheIter = _cache.begin();
}

SequentialDocumentCache::CacheStatus SequentialDocumentCache::checkCacheSize(const Document& doc) {
    if (_sizeBytes + doc.getApproximateSize() > _maxSizeBytes) {
        abandon();
    }

    return _status;
}

}  // namespace mongo
