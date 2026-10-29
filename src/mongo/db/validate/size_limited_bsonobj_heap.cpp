// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/size_limited_bsonobj_heap.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/util/assert_util.h"

#include <algorithm>

namespace mongo {
bool SizeLimitedBSONObjHeap::add(BSONObj obj) {
    invariant(obj.objsize() > 0);

    // Add the BSONObj to the heap
    _usedBytes += static_cast<size_t>(obj.objsize());
    _storage.push_back(std::move(obj));
    std::push_heap(_storage.begin(), _storage.end(), LargestObjsPopFirstCmp{});
    if (_usedBytes <= _maxBytes || _storage.size() <= 1) {
        return false;
    }
    // evict the largest object otherwise, to keep the total size under the limit
    std::pop_heap(_storage.begin(), _storage.end(), LargestObjsPopFirstCmp{});
    const BSONObj& evictedObj = _storage.back();
    _usedBytes -= evictedObj.objsize();
    _storage.pop_back();
    return true;
}

void SizeLimitedBSONObjHeap::clear() {
    _storage.clear();
    _usedBytes = 0;
}
}  // namespace mongo
