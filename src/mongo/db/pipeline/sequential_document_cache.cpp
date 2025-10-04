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

#include "mongo/db/pipeline/sequential_document_cache.h"

#include "mongo/util/assert_util.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void SequentialDocumentCache::add(Document doc) {
    invariant(_status == CacheStatus::kBuilding);

    if (checkCacheSize(doc) != CacheStatus::kAbandoned) {
        _sizeBytes += doc.getApproximateSize();
        _cache.push_back(std::move(doc));
    }
}

void SequentialDocumentCache::freeze() {
    invariant(_status == CacheStatus::kBuilding);

    _status = CacheStatus::kServing;
    _cache.shrink_to_fit();

    _cacheIter = _cache.begin();
}

void SequentialDocumentCache::abandon() {
    _status = CacheStatus::kAbandoned;

    _cache.clear();
    _cache.shrink_to_fit();

    _cacheIter = _cache.begin();
}

boost::optional<Document> SequentialDocumentCache::getNext() {
    invariant(_status == CacheStatus::kServing);

    if (_cacheIter == _cache.end()) {
        return boost::none;
    }

    return *_cacheIter++;
}

void SequentialDocumentCache::restartIteration() {
    invariant(_status == CacheStatus::kServing);
    _cacheIter = _cache.begin();
}

SequentialDocumentCache::CacheStatus SequentialDocumentCache::checkCacheSize(const Document& doc) {
    if (_sizeBytes + doc.getApproximateSize() > _maxSizeBytes) {
        abandon();
    }

    return _status;
}

}  // namespace mongo
