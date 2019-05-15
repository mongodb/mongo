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

#pragma once

namespace mongo {

class OperationContext;

/**
 * Function helpers to manipulate parameters affecting the snapshot time window size that the
 * storage engine maintains of available snapshots for point-in-time reads.
 */
namespace SnapshotWindowUtil {

/**
 * Increases the setting that controls the window of time between stable_timestamp and
 * oldest_timestamp, in order to provide a greater range of available snapshots for point-in-time
 * operations. This function will be called when server requests return SnapshotTooOld (or similar)
 * errors. Note that this will not immediately affect the oldest_timestamp. Rather, it affects
 * actions taken next time oldest_timestamp is updated, usually when the stable timestamp is
 * advanced.
 *
 * Implements an additive increase algorithm.
 *
 * Calling many times all at once has the same effect as calling once. The last update time is
 * tracked and attempts to increase the window are limited to once in
 * minMillisBetweenSnapshotWindowInc. This is to protect against a sudden wave of function calls due
 * to simultaneous SnapshotTooOld errors. Some time must be allowed for the increased target
 * snapshot window size setting to have an effect. The target size can also never exceed
 * maxTargetSnapshotHistoryWindowInSeconds.
 */
void increaseTargetSnapshotWindowSize(OperationContext* opCtx);

/**
 * Decreases the target snapshot window size if there has been cache pressure and no new
 * SnapshotTooOld errors as tracked via the incrementSnapshotTooOldErrorCount() below since the last
 * time this function was called.
 *
 * We do not decrease the window size when there have been SnapshotTooOld errors recently, even if
 * there is some cache pressure, because that will cause sharded transaction commands to fail.
 *
 * Note that this will not necessarily immediately affect the actual window size; rather,
 * it affects actions taken whenever oldest_timestamp is updated, usually when the stable_timestamp
 * is advanced.
 *
 * This will make one attempt to immediately adjust the window size if possible.
 *
 * Implements a multiplicative decrease algorithm.
 */
void decreaseTargetSnapshotWindowSize(OperationContext* opCtx);

/**
 * Registers on a counter SnapshotTooOld errors encountered in the command layer.
 * decreaseTargetSnapshotWindowSize() above uses the counter value: if there are any errors since
 * the last decreaseTargetSnapshotWindowSize() call, then decreaseTargetSnapshotWindowSize() will
 * choose not decrease the target snapshot window size setting.
 *
 * Concurrency safe, the internal counter is atomic.
 */
void incrementSnapshotTooOldErrorCount();

}  // namespace SnapshotWindowUtil
}  // namespace mongo
