/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_read.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

namespace mongo::replicated_fast_count {

CollectionSizeCount readLatest(OperationContext* opCtx,
                               const SizeCountStore& sizeCountStore,
                               const SizeCountTimestampStore& timestampStore,
                               SeekableRecordCursor& cursor,
                               UUID uuid) {
    const auto entry = sizeCountStore.read(opCtx, uuid);
    massert(12092100,
            fmt::format("Expected the size/count store to contain an entry for UUID={}",
                        uuid.toString()),
            entry.has_value());
    // We default to Timestamp::min() when the timestamp store does not yet contain a timestamp.
    const Timestamp timestamp = timestampStore.read(opCtx).value_or(Timestamp::min());

    // We compute the maximum of the timestamp in the size count store and the timestamp in the
    // timestamp store to determine where to begin reading the oplog. The size and count entry is
    // valid as of the maximum timestamp, so we only need to aggregate deltas after that point in
    // time.
    const Timestamp seekAfterTs = std::max(entry->timestamp, timestamp);
    const auto deltas = aggregateSizeCountDeltasInOplog(cursor, seekAfterTs, uuid).deltas;

    // If there are no oplog entries for this UUID after seekAfterTs, the stored values are already
    // accurate and no delta adjustment is needed.
    if (!deltas.contains(uuid)) {
        return CollectionSizeCount{.size = entry->size, .count = entry->count};
    }

    return CollectionSizeCount{.size = entry->size + deltas.at(uuid).size,
                               .count = entry->count + deltas.at(uuid).count};
}

[[nodiscard]] CollectionSizeCount readPersisted(OperationContext* opCtx,
                                                const SizeCountStore& sizeCountStore,
                                                UUID uuid) {
    const auto entry = sizeCountStore.read(opCtx, uuid);
    massert(12282000,
            fmt::format("Expected the size/count store to contain an entry for UUID={}",
                        uuid.toString()),
            entry.has_value());
    return CollectionSizeCount{.size = entry->size, .count = entry->count};
}
}  // namespace mongo::replicated_fast_count
