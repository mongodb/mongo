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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/storage_parameters_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
using IndexIterator = IndexCatalog::IndexIterator;
using ReadyIndexesIterator = IndexCatalog::ReadyIndexesIterator;
using AllIndexesIterator = IndexCatalog::AllIndexesIterator;

bool IndexIterator::more() {
    if (_start) {
        _next = _advance();
        _start = false;
    }
    return _next != nullptr;
}

const IndexCatalogEntry* IndexIterator::next() {
    if (!more())
        return nullptr;
    _prev = _next;
    _next = _advance();
    return _prev;
}

ReadyIndexesIterator::ReadyIndexesIterator(OperationContext* const opCtx,
                                           IndexCatalogEntryContainer::const_iterator beginIterator,
                                           IndexCatalogEntryContainer::const_iterator endIterator)
    : _opCtx(opCtx), _iterator(beginIterator), _endIterator(endIterator) {}

const IndexCatalogEntry* ReadyIndexesIterator::_advance() {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    auto pitFeatureEnabled =
        feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe();
    while (_iterator != _endIterator) {
        IndexCatalogEntry* entry = _iterator->get();
        ++_iterator;

        // When the PointInTimeCatalogLookups feature flag is not enabled, it is necessary to check
        // whether the operation's read timestamp is before or after the most recent index
        // modification (indicated by the minimum visible snapshot on the IndexCatalogEntry). If the
        // read timestamp is before the most recent index modification, we must not include this
        // entry in the iterator.
        //
        // When the PointInTimeCatalogLookups feature flag is enabled, the index catalog entry will
        // be constructed from the durable catalog for reads with a read timestamp older than the
        // minimum valid snapshot for the collection (which reflects the most recent catalog
        // modification for that collection, including index modifications), so there's no need to
        // check the minimum visible snapshot of the entry here.
        if (!pitFeatureEnabled) {
            if (auto minSnapshot = entry->getMinimumVisibleSnapshot()) {
                auto mySnapshot =
                    _opCtx->recoveryUnit()->getPointInTimeReadTimestamp(_opCtx).get_value_or(
                        _opCtx->recoveryUnit()->getCatalogConflictingTimestamp());

                if (!mySnapshot.isNull() && mySnapshot < minSnapshot.value()) {
                    // This index isn't finished in my snapshot.
                    continue;
                }
            }
        }

        return entry;
    }

    return nullptr;
}

AllIndexesIterator::AllIndexesIterator(
    OperationContext* const opCtx, std::unique_ptr<std::vector<IndexCatalogEntry*>> ownedContainer)
    : _opCtx(opCtx), _ownedContainer(std::move(ownedContainer)) {
    // Explicitly order calls onto the ownedContainer with respect to its move.
    _iterator = _ownedContainer->begin();
    _endIterator = _ownedContainer->end();
}

const IndexCatalogEntry* AllIndexesIterator::_advance() {
    if (_iterator == _endIterator) {
        return nullptr;
    }

    IndexCatalogEntry* entry = *_iterator;
    ++_iterator;
    return entry;
}

StringData toString(IndexBuildMethod method) {
    switch (method) {
        case IndexBuildMethod::kHybrid:
            return "Hybrid"_sd;
        case IndexBuildMethod::kForeground:
            return "Foreground"_sd;
    }

    MONGO_UNREACHABLE;
}
}  // namespace mongo
