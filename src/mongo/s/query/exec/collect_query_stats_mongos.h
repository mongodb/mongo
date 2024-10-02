/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/s/query/exec/cluster_cursor_manager.h"

namespace mongo {

/**
 * Record metrics for the current operation on opDebug and aggregates those metrics for queryStats
 * use. If a cursor is provided (via ClusterClientCursorGuard or
 * ClusterCursorManager::PinnedCursor), metrics are aggregated on the cursor; otherwise, metrics are
 * written directly to the queryStats store.
 * NOTE: Metrics are taken from opDebug.additiveMetrics, so CurOp::setEndOfOpMetrics must be called
 * *prior* to calling these.
 *
 * Currently, queryStats is only collected for find and aggregate requests (and their subsequent
 * getMore requests), so these should only be called from those request paths.
 */
void collectQueryStatsMongos(OperationContext* opCtx, std::unique_ptr<query_stats::Key> key);
void collectQueryStatsMongos(OperationContext* opCtx, ClusterClientCursorGuard& cursor);
void collectQueryStatsMongos(OperationContext* opCtx, ClusterCursorManager::PinnedCursor& cursor);

}  // namespace mongo
