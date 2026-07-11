// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"

namespace mongo::replicated_fast_count {

/**
 * Returns the replicated fast count timestamp store's "valid-as-of" timestamp written by
 * 'oplogEntry'.
 *
 * The entry may be a top-level container op, or a container op nested inside the 'applyOps' array
 * of an applyOps command oplog entry.
 */
[[MONGO_MOD_PUBLIC]] Timestamp getTimestampStoreValidAsOfFromOplogEntry(const BSONObj& oplogEntry);

/**
 * Returns the find query filter that selects oplog entries writing to the replicated fast count
 * timestamp store either as a top-level container op (matched on the 'container' field) or nested
 * inside an applyOps command entry (matched on the 'o.applyOps.container' dotted path). We never do
 * deletes to the timestamp store, so we don't need to filter on specific opType. Used during
 * initial sync.
 */
[[MONGO_MOD_PUBLIC]] BSONObj fastCountValidAsOfScanFilter();

}  // namespace mongo::replicated_fast_count
