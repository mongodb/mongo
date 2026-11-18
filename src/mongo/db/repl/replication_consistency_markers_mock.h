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

    void createRecordIdsGroupIdCollection(OperationContext* opCtx) override;
    void setRecordIdsGroupId(OperationContext* opCtx,
                             const UUID& groupId,
                             const OpTime& foundingOpTime) override;
    boost::optional<RecordIdsGroupIdDocument> getRecordIdsGroupId(OperationContext* opCtx) override;
    boost::optional<RecordIdsGroupMembership> getEffectiveRecordIdsGroupId(
        OperationContext* opCtx) const override;
    void reloadEffectiveRecordIdsGroupId(OperationContext* opCtx) override;
    void clearRecordIdsGroupId(OperationContext* opCtx) override;

    /**
     * Replaces the stored recordIdsGroupId document without touching the effective membership, as
     * recovering to the stable timestamp or swapping in another node's files would. Lets tests
     * check that a caller reloads the effective membership after such a change.
     */
    void replaceStoredRecordIdsGroupId_forTest(
        const boost::optional<RecordIdsGroupMembership>& membership);

private:
    mutable std::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable std::mutex _minValidBoundariesMutex;
    OpTime _appliedThrough;
    OpTime _minValid;
    Timestamp _oplogTruncateAfterPoint;
    BSONObj _initialSyncId;

    // Guards both the stored document and the effective membership below.
    mutable std::mutex _recordIdsGroupIdMutex;
    // Stands in for the on-disk recordIdsGroupId document.
    boost::optional<RecordIdsGroupIdDocument> _recordIdsGroupId;
    // Stands in for ReplicationConsistencyMarkersImpl's in-memory effective membership
    // (boost::none: not a member). It is kept separate from the stored document only so that
    // tests can detect a caller that fails to reload after the stored document changes behind it
    // (see replaceStoredRecordIdsGroupId_forTest). Otherwise, this does not fully mock the Impl's
    // cache:
    //  - It starts loaded, as not a member, matching the empty stored document, so reading it
    //    before any reload does not tassert as the Impl does.
    //  - There is no WriteUnitOfWork, so setRecordIdsGroupId and clearRecordIdsGroupId update it
    //    immediately rather than on commit, and nothing models an aborted write.
    //  - It ignores the persistence provider, so it never reports the sentinel membership that the
    //    Impl returns when the provider requires replicated RecordIds.
    boost::optional<RecordIdsGroupMembership> _effectiveRecordIdsGroupId;
};

}  // namespace repl
}  // namespace mongo
