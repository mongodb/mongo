
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_descriptor.h"


namespace mongo {

IndexCatalog::IndexIterator::IndexIterator(OperationContext* opCtx,
                                           IndexCatalogEntryContainer::const_iterator beginIterator,
                                           IndexCatalogEntryContainer::const_iterator endIterator,
                                           bool includeUnfinishedIndexes)
    : _includeUnfinishedIndexes(includeUnfinishedIndexes),
      _opCtx(opCtx),
      _iterator(beginIterator),
      _endIterator(endIterator),
      _start(true),
      _prev(nullptr),
      _next(nullptr) {}

bool IndexCatalog::IndexIterator::more() {
    if (_start) {
        _advance();
        _start = false;
    }
    return _next != nullptr;
}

IndexDescriptor* IndexCatalog::IndexIterator::next() {
    if (!more())
        return nullptr;
    _prev = _next;
    _advance();
    return _prev->descriptor();
}

IndexAccessMethod* IndexCatalog::IndexIterator::accessMethod(const IndexDescriptor* desc) {
    invariant(desc == _prev->descriptor());
    return _prev->accessMethod();
}

IndexCatalogEntry* IndexCatalog::IndexIterator::catalogEntry(const IndexDescriptor* desc) {
    invariant(desc == _prev->descriptor());
    return _prev;
}

void IndexCatalog::IndexIterator::_advance() {
    _next = nullptr;

    while (_iterator != _endIterator) {
        IndexCatalogEntry* entry = _iterator->get();
        ++_iterator;

        if (!_includeUnfinishedIndexes) {
            if (auto minSnapshot = entry->getMinimumVisibleSnapshot()) {
                if (auto mySnapshot = _opCtx->recoveryUnit()->getPointInTimeReadTimestamp()) {
                    if (mySnapshot < minSnapshot) {
                        // This index isn't finished in my snapshot.
                        continue;
                    }
                }
            }

            if (!entry->isReady(_opCtx))
                continue;
        }

        _next = entry;
        return;
    }
}

}  // namespace mongo
