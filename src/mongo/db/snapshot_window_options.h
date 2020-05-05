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

#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/platform/atomic_proxy.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * These are parameters that affect how much snapshot history the storage engine will keep to
 * support snapshot reads. This is referred to as the snapshot window. The window is between the
 * stable timestamp and the oldest timestamp.
 */
struct SnapshotWindowParams {

    // minSnapshotHistoryWindowInSeconds (startup & runtime server parameter, range 0+).
    //
    // Dictates the lag in seconds oldest_timestamp should be set behind stable_timestamp.
    //
    // Note that the window size can become greater than this if an ongoing operation is holding an
    // older snapshot open.
    AtomicWord<int> minSnapshotHistoryWindowInSeconds{5};

    // cachePressureThreshold (startup & runtime server parameter, range [0, 100]).
    //
    // Compares against a storage engine cache pressure indicator that ranges from 0 to 100.
    // Currently, the only indicator is the WT lookaside score.
    AtomicWord<int> cachePressureThreshold{95};

    AtomicWord<std::int64_t> snapshotTooOldErrorCount{0};
};

extern SnapshotWindowParams snapshotWindowParams;

}  // namespace mongo
