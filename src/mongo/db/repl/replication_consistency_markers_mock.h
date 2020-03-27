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

#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/platform/mutex.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

/**
 * A mock ReplicationConsistencyMarkers implementation that stores everything in memory.
 */
class ReplicationConsistencyMarkersMock : public ReplicationConsistencyMarkers {
    ReplicationConsistencyMarkersMock(const ReplicationConsistencyMarkersMock&) = delete;
    ReplicationConsistencyMarkersMock& operator=(const ReplicationConsistencyMarkersMock&) = delete;

public:
    ReplicationConsistencyMarkersMock() = default;

    void initializeMinValidDocument(OperationContext* opCtx) override;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;
    void setInitialSyncFlag(OperationContext* opCtx) override;
    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) override;

    void ensureFastCountOnOplogTruncateAfterPoint(OperationContext* opCtx) override;

    void setOplogTruncateAfterPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const override;

    void startUsingOplogTruncateAfterPointForPrimary() override;
    void stopUsingOplogTruncateAfterPointForPrimary() override;
    bool isOplogTruncateAfterPointBeingUsedForPrimary() const override;

    void setOplogTruncateAfterPointToTopOfOplog(OperationContext* opCtx) override;

    boost::optional<OpTimeAndWallTime> refreshOplogTruncateAfterPointIfPrimary(
        OperationContext* opCtx) override;

    void setAppliedThrough(OperationContext* opCtx,
                           const OpTime& optime,
                           bool setTimestamp = true) override;
    void clearAppliedThrough(OperationContext* opCtx, const Timestamp& writeTimestamp) override;
    OpTime getAppliedThrough(OperationContext* opCtx) const override;

    Status createInternalCollections(OperationContext* opCtx) override;

    void setInitialSyncIdIfNotSet(OperationContext* opCtx) override;
    void clearInitialSyncId(OperationContext* opCtx) override;
    BSONObj getInitialSyncId(OperationContext* opCtx) override;

private:
    mutable Mutex _initialSyncFlagMutex =
        MONGO_MAKE_LATCH("ReplicationConsistencyMarkersMock::_initialSyncFlagMutex");
    bool _initialSyncFlag = false;

    mutable Mutex _minValidBoundariesMutex =
        MONGO_MAKE_LATCH("ReplicationConsistencyMarkersMock::_minValidBoundariesMutex");
    OpTime _appliedThrough;
    OpTime _minValid;
    Timestamp _oplogTruncateAfterPoint;
};

}  // namespace repl
}  // namespace mongo
