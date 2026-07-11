// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/util/modules.h"

#include <functional>
#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace [[MONGO_MOD_PUBLIC]] repl {

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

    void ensureFastCountOnOplogTruncateAfterPoint(OperationContext* opCtx) override;

    void setOplogTruncateAfterPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const override;

    void startUsingOplogTruncateAfterPointForPrimary() override;
    void stopUsingOplogTruncateAfterPointForPrimary() override;
    bool isOplogTruncateAfterPointBeingUsedForPrimary() const override;

    void setOplogTruncateAfterPointToTopOfOplog(OperationContext* opCtx) override;

    boost::optional<OpTimeAndWallTime> refreshOplogTruncateAfterPointIfPrimary(
        OperationContext* opCtx) override;

    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    void clearAppliedThrough(OperationContext* opCtx) override;
    OpTime getAppliedThrough(OperationContext* opCtx) const override;
    mutable std::function<void(OperationContext*)> getAppliedThroughFn;

    Status createInternalCollections(OperationContext* opCtx) override;

    void setInitialSyncIdIfNotSet(OperationContext* opCtx) override;
    void clearInitialSyncId(OperationContext* opCtx) override;
    BSONObj getInitialSyncId(OperationContext* opCtx) override;

private:
    mutable std::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable std::mutex _minValidBoundariesMutex;
    OpTime _appliedThrough;
    OpTime _minValid;
    Timestamp _oplogTruncateAfterPoint;
    BSONObj _initialSyncId;
};

}  // namespace repl
}  // namespace mongo
