/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/snapshot_window_util.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/snapshot_window_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

// This failpoint is used for performance testing.
MONGO_FAIL_POINT_DEFINE(preventDynamicSnapshotHistoryWindowTargetAdjustments);

namespace SnapshotWindowUtil {

// Adds concurrency control to increaseTargetSnapshotWindowSize() and
// decreaseTargetSnapshotWindowSize(). They should not run concurrently with themselves or one
// another, since they act on and modify the same storage parameters. Further guards the static
// variables "_snapshotWindowLastDecreasedAt" and "_snapshotWindowLastIncreasedAt" used in
// increaseTargetSnapshotWindowSize() and decreaseSnapshowWindow().
stdx::mutex snapshotWindowMutex;

namespace {

void _decreaseTargetSnapshotWindowSize(WithLock lock, OperationContext* opCtx) {
    // Tracks the last time that the snapshot window was decreased so that it does not go down so
    // fast that the system does not have time to react and reduce snapshot availability.
    static Date_t _snapshotWindowLastDecreasedAt{Date_t::min()};

    if (_snapshotWindowLastDecreasedAt >
        (Date_t::now() -
         Milliseconds(snapshotWindowParams.minMillisBetweenSnapshotWindowDec.load()))) {
        // We have already decreased the window size in the last minMillisBetweenSnapshotWindowDec
        // milliseconds.
        return;
    }

    snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.store(
        snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load() *
        snapshotWindowParams.snapshotWindowMultiplicativeDecrease.load());

    // Try to set the oldest_timestamp immediately without waiting for a write to adjust the
    // window. May or may not work depending on the state of the system.
    StorageEngine* engine = opCtx->getServiceContext()->getStorageEngine();
    invariant(engine);
    engine->setOldestTimestampFromStable();

    _snapshotWindowLastDecreasedAt = Date_t::now();
}

}  // namespace

void increaseTargetSnapshotWindowSize(OperationContext* opCtx) {
    if (MONGO_FAIL_POINT(preventDynamicSnapshotHistoryWindowTargetAdjustments)) {
        return;
    }

    stdx::unique_lock<stdx::mutex> lock(snapshotWindowMutex);

    // Tracks the last time that the snapshot window was increased so that it does not go up so fast
    // that the storage engine does not have time to improve snapshot availability.
    static Date_t _snapshotWindowLastIncreasedAt{Date_t::min()};

    if (_snapshotWindowLastIncreasedAt >
        (Date_t::now() -
         Milliseconds(snapshotWindowParams.minMillisBetweenSnapshotWindowInc.load()))) {
        // We have already increased the window size in the last minMillisBetweenSnapshotWindowInc
        // milliseconds.
        return;
    }

    // If the cache pressure is already too high, we will not put more pressure on it by increasing
    // the window size.
    StorageEngine* engine = opCtx->getServiceContext()->getStorageEngine();
    if (engine && engine->isCacheUnderPressure(opCtx)) {
        warning() << "Attempted to increase the time window of available snapshots for "
                     "point-in-time operations (readConcern level 'snapshot' or transactions), but "
                     "the storage engine cache pressure, per the cachePressureThreshold setting of "
                     "'"
                  << snapshotWindowParams.cachePressureThreshold.load()
                  << "', is too high to allow it to increase. If this happens frequently, consider "
                     "either increasing the cache pressure threshold or increasing the memory "
                     "available to the storage engine cache, in order to improve the success rate "
                     "or speed of point-in-time requests.";
        _decreaseTargetSnapshotWindowSize(lock, opCtx);
        return;
    }

    if (snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load() ==
        snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load()) {
        warning() << "Attempted to increase the time window of available snapshots for "
                     "point-in-time operations (readConcern level 'snapshot' or transactions), but "
                     "maxTargetSnapshotHistoryWindowInSeconds has already been reached. If this "
                     "happens frequently, consider increasing the "
                     "maxTargetSnapshotHistoryWindowInSeconds setting value, which is currently "
                     "set to '"
                  << snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load() << "'.";
        return;
    }

    int increasedSnapshotWindow = snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.load() +
        snapshotWindowParams.snapshotWindowAdditiveIncreaseSeconds.load();
    snapshotWindowParams.targetSnapshotHistoryWindowInSeconds.store(
        std::min(increasedSnapshotWindow,
                 snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds.load()));

    _snapshotWindowLastIncreasedAt = Date_t::now();
}

void decreaseTargetSnapshotWindowSize(OperationContext* opCtx) {
    if (MONGO_FAIL_POINT(preventDynamicSnapshotHistoryWindowTargetAdjustments)) {
        return;
    }

    stdx::unique_lock<stdx::mutex> lock(snapshotWindowMutex);

    StorageEngine* engine = opCtx->getServiceContext()->getStorageEngine();
    if (engine && engine->isCacheUnderPressure(opCtx)) {
        _decreaseTargetSnapshotWindowSize(lock, opCtx);
    }
}

}  // namespace SnapshotWindowUtil
}  // namespace mongo
