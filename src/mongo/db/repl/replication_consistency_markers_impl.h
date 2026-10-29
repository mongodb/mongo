// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

class OpTime;
class StorageInterface;
struct TimestampedBSONObj;

class ReplicationConsistencyMarkersImpl : public ReplicationConsistencyMarkers {
    ReplicationConsistencyMarkersImpl(const ReplicationConsistencyMarkersImpl&) = delete;
    ReplicationConsistencyMarkersImpl& operator=(const ReplicationConsistencyMarkersImpl&) = delete;

public:
    explicit ReplicationConsistencyMarkersImpl(StorageInterface* storageInterface);
    ReplicationConsistencyMarkersImpl(StorageInterface* storageInterface,
                                      NamespaceString minValidNss,
                                      NamespaceString oplogTruncateAfterNss,
                                      NamespaceString initialSyncIdNss);

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

    Status createInternalCollections(OperationContext* opCtx) override;

    void setInitialSyncIdIfNotSet(OperationContext* opCtx) override;
    void clearInitialSyncId(OperationContext* opCtx) override;
    BSONObj getInitialSyncId(OperationContext* opCtx) override;

private:
    /**
     * Reads the MinValid document from disk.
     * Returns boost::none if not present.
     */
    boost::optional<MinValidDocument> _getMinValidDocument(OperationContext* opCtx) const;

    /**
     * Updates the MinValid document according to the provided update spec. The collection must
     * exist, see `createInternalCollections`. If the document does not exist, it is upserted.
     *
     * This fasserts on failure.
     */
    void _updateMinValidDocument(OperationContext* opCtx, const BSONObj& updateSpec);

    /**
     * Reads the OplogTruncateAfterPoint document from disk.
     * Returns boost::none if not present.
     */
    boost::optional<OplogTruncateAfterPointDocument> _getOplogTruncateAfterPointDocument(
        OperationContext* opCtx) const;

    /**
     * Updates the oplogTruncateAfterPoint with 'timestamp'. Callers should use this codepath when
     * expecting write interruption errors.
     */
    Status _setOplogTruncateAfterPoint(const CollectionPtr& collection,
                                       OperationContext* opCtx,
                                       const Timestamp& timestamp);

    /**
     * Upserts the OplogTruncateAfterPoint document according to the provided update spec. The
     * collection must already exist. See `createInternalCollections`.
     */
    Status _upsertOplogTruncateAfterPointDocument(const CollectionPtr& collection,
                                                  OperationContext* opCtx,
                                                  const BSONObj& updateSpec);

    StorageInterface* _storageInterface;
    const NamespaceString _minValidNss;
    const NamespaceString _oplogTruncateAfterPointNss;
    const NamespaceString _initialSyncIdNss;

    // Protects modifying and reading _isPrimary below.
    mutable std::mutex _truncatePointIsPrimaryMutex;

    // Tracks whether or not the node is primary. Avoids potential deadlocks taking the replication
    // coordinator's mutex to check replication state. Also remains false for standalones that do
    // not use timestamps.
    bool _isPrimary = false;

    // Locks around fetching the 'all_durable' timestamp from the storage engine and updating the
    // oplogTruncateAfterPoint. This prevents the oplogTruncateAfterPoint from going backwards in
    // time in case of multiple callers to refreshOplogTruncateAfterPointIfPrimary.
    mutable std::mutex _refreshOplogTruncateAfterPointMutex;

    // In-memory cache of the of the oplog entry LTE to the oplogTruncateAfterPoint timestamp.
    // Eventually matches the oplogTruncateAfterPoint timestamp when parallel writes finish. Avoids
    // repeatedly writing the same oplogTruncateAfterPoint timestamp to disk, which creates noise in
    // a silent system. Only set in state PRIMARY.
    //
    // Reset whenever setOplogTruncateAfterPoint() manually resets the truncate point: this could
    // push the durable truncate point forwards or backwards in time, reflecting changes in the
    // oplog. The truncate point is manually set in non-PRIMARY states.
    //
    // Note: these values lack their own specific concurrency control, instead depending on the
    // serialization that exists in setting the oplog truncate after point.
    boost::optional<Timestamp> _lastNoHolesOplogTimestamp;
    boost::optional<OpTimeAndWallTime> _lastNoHolesOplogOpTimeAndWallTime;

    // Cached initialSyncId from last initial sync. Will only be set on startup or initial sync.
    BSONObj _initialSyncId;

    // Cached recordId of the oplogTruncateAfterPoint to speed up subsequent updates
    boost::optional<RecordId> _oplogTruncateRecordId;
};

}  // namespace repl
}  // namespace mongo
