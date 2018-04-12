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

#pragma once

#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * This is a collection of parameters that affect how much snapshot history the storage engine will
 * maintain to support point-in-time transactions (read or write). This is referred to as the
 * snapshot window.
 */
struct SnapshotWindowParams {

    // maxTargetSnapshotHistoryWindowInSeconds (startup & runtime server paramter, range 0+).
    //
    // Dictates the maximum lag in seconds oldest_timestamp should be behind stable_timestamp.
    // targetSnapshotHistoryWindowInSeconds below is the actual active lag setting target.
    //
    // Note that the window size can become greater than this if an ongoing operation is holding an
    // older snapshot open.
    AtomicInt32 maxTargetSnapshotHistoryWindowInSeconds{100};

    // targetSnapshotHistoryWindowInSeconds (not a server parameter, range 0+).
    //
    // Dictates the target lag in seconds oldest_timestamp should be set behind stable_timestamp.
    // Should only be set in the range [0, maxTargetSnapshotHistoryWindowInSeconds].
    //
    // Note that this is the history window we attempt to maintain, but our current system state may
    // not always reflect it: the window can only change as more writes come in, so it can take time
    // for the actual window size to catch up with a change. This value guides actions whenever the
    // system goes to update the oldest_timestamp value.
    AtomicInt32 targetSnapshotHistoryWindowInSeconds{
        maxTargetSnapshotHistoryWindowInSeconds.load()};

    // cachePressureThreshold (startup & runtime server paramter, range [0, 100]).
    //
    // Dictates what percentage of cache in use is considered too high. This setting helps preempt
    // storage cache pressure immobilizing the system. Attempts to increase
    // targetSnapshotHistoryWindowInSeconds will be ignored when the cache pressure reaches this
    // threshold. Additionally, a periodic task will decrease targetSnapshotHistoryWindowInSeconds
    // when cache pressure exceeds the threshold.
    AtomicInt32 cachePressureThreshold{50};

    // snapshotWindowMultiplicativeDecrease (startup & runtime server paramter, range (0,1)).
    //
    // Controls by what multiplier the target snapshot history window setting is decreased when
    // cache pressure becomes too high, per the cachePressureThreshold setting.
    AtomicDouble snapshotWindowMultiplicativeDecrease{0.75};

    // snapshotWindowAdditiveIncreaseSeconds (startup & runtime server paramter, range 1+).
    //
    // Controls by how much the target snapshot history window setting is increased when cache
    // pressure is OK, per cachePressureThreshold, and we need to service older snapshots for global
    // point-in-time reads.
    AtomicInt32 snapshotWindowAdditiveIncreaseSeconds{2};

    // minMillisBetweenSnapshotWindowInc (startup & runtime server paramter, range 0+).
    // minMillisBetweenSnapshotWindowDec (startup & runtime server paramter, range 0+).
    //
    // Controls how often attempting to increase/decrease the target snapshot window will have an
    // effect. Multiple callers within minMillisBetweenSnapshotWindowInc will have the same effect
    // as one. This protects the system because it takes time for the target snapshot window to
    // affect the actual storage engine snapshot window. The stable timestamp must move forward for
    // the window between it and oldest timestamp to grow or shrink.
    AtomicInt32 minMillisBetweenSnapshotWindowInc{500};
    AtomicInt32 minMillisBetweenSnapshotWindowDec{500};

    // checkCachePressurePeriodSeconds (startup & runtime server paramter, range 1+)
    //
    // Controls the period of the task that checks for cache pressure and decreases
    // targetSnapshotHistoryWindowInSeconds if the pressure is above cachePressureThreshold. The
    // target window size setting must not be decreased too fast because time must be allowed for
    // the storage engine to attempt to act on the new setting.
    AtomicInt32 checkCachePressurePeriodSeconds{5};
};

extern SnapshotWindowParams snapshotWindowParams;

}  // namespace mongo
