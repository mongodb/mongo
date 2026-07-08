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
MONGO_MOD_PUBLIC Timestamp getTimestampStoreValidAsOfFromOplogEntry(const BSONObj& oplogEntry);

/**
 * Returns the find query filter that selects oplog entries writing to the replicated fast count
 * timestamp store either as a top-level container op (matched on the 'container' field) or nested
 * inside an applyOps command entry (matched on the 'o.applyOps.container' dotted path). We never do
 * deletes to the timestamp store, so we don't need to filter on specific opType. Used during
 * initial sync.
 */
MONGO_MOD_PUBLIC BSONObj fastCountValidAsOfScanFilter();

}  // namespace mongo::replicated_fast_count
