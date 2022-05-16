/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/derived_metadata_collection_count.h"

#include "mongo/bson/bsonobj.h"

namespace mongo {
const auto getDerivedCountDecoration =
    SharedCollectionDecorations::declareDecoration<DerivedMetadataCount>();

DerivedMetadataCount& DerivedMetadataCount::get(const Collection* coll) {
    return getDerivedCountDecoration(coll->getSharedDecorations());
}

std::pair<Timestamp, long long> DerivedMetadataCount::getLatestValue() {
    stdx::lock_guard lock(_valueMutex);
    return _latestTimestampedValue;
}

void DerivedMetadataCount::populateDelta(const DocumentWriteImages& write,
                                         DerivedMetadataDelta* delta) {
    if (write.documentPreImage.isEmpty() && !write.documentPostImage.isEmpty()) {
        // Inserting a document so increment the count.
        delta->countUpdate = 1;
    } else if (!write.documentPreImage.isEmpty() && write.documentPostImage.isEmpty()) {
        // Deleting a document so decrement the count.
        delta->countUpdate = -1;
    } else {
        delta->countUpdate = 0;
    }
}

void DerivedMetadataCount::applyDeltas(const std::vector<DerivedMetadataDelta>& deltas) {
    stdx::lock_guard lock(_valueMutex);
    const auto& latestTimestamp = _latestTimestampedValue.first;
    long long deltaSum = _latestTimestampedValue.second;

    Timestamp newMaxTimestamp = Timestamp(0);
    for (auto& delta : deltas) {
        // Ignore Deltas with Timestamps less than or equal to the latest Timestamp.
        if (delta.timestamp <= latestTimestamp) {
            continue;
        }
        newMaxTimestamp = (newMaxTimestamp < delta.timestamp) ? delta.timestamp : newMaxTimestamp;
        deltaSum += delta.countUpdate;
    }

    // If none of the Deltas were new, we can skip updating the latest value.
    if (newMaxTimestamp <= latestTimestamp) {
        return;
    }

    _updateLatestValue(lock, std::move(newMaxTimestamp), deltaSum);
}

void DerivedMetadataCount::_updateLatestValue(WithLock,
                                              Timestamp newTimestamp,
                                              long long newCount) {
    _latestTimestampedValue = {std::move(newTimestamp), newCount};
}
}  // namespace mongo
