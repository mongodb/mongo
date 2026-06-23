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

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/logical_size_snapshot_gen.h"

namespace mongo {

/**
 * Enables tracking of the total logical size (bytes) across all collections.
 */
class LogicalSizeTracker {
public:
    /**
     * Returns a snapshot of the total logical bytes for hot and cold collections across the
     * node.
     *
     * Avoids excessive locking beyond taking the GlobalLock. Results are subject to staleness.
     */
    LogicalSizeSnapshot getLatestSnapshot() const;

    void refreshLatestSnapshot_ForTest(OperationContext* opCtx) {
        _refreshLatestSnapshot(opCtx);
    }

private:
    /**
     * Computes a new `latesetSnapshot` according to the cached data sizes across collections.
     */
    void _refreshLatestSnapshot(OperationContext* opCtx);


    /**
     * TODO SERVER-128941: Introduced concurrency control and background job semantics.
     */
    LogicalSizeSnapshot _latestSnapshot;
};

}  // namespace mongo

