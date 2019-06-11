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

#include "mongo/base/status_with.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class OperationContext;

namespace repl {

class StorageInterface;
class ReplicationConsistencyMarkers;

/**
 * This class is used by the replication system to recover after an unclean shutdown or a rollback.
 */
class ReplicationRecovery {
public:
    ReplicationRecovery() = default;
    virtual ~ReplicationRecovery() = default;

    /**
     * Recovers the data on disk from the oplog. If the provided stable timestamp is not "none",
     * this function assumes the data reflects that timestamp.
     */
    virtual void recoverFromOplog(OperationContext* opCtx,
                                  boost::optional<Timestamp> stableTimestamp) = 0;
};

class ReplicationRecoveryImpl : public ReplicationRecovery {
    ReplicationRecoveryImpl(const ReplicationRecoveryImpl&) = delete;
    ReplicationRecoveryImpl& operator=(const ReplicationRecoveryImpl&) = delete;

public:
    ReplicationRecoveryImpl(StorageInterface* storageInterface,
                            ReplicationConsistencyMarkers* consistencyMarkers);

    void recoverFromOplog(OperationContext* opCtx,
                          boost::optional<Timestamp> stableTimestamp) override;

private:
    /**
     * After truncating the oplog, completes recovery if we're recovering from a stable timestamp
     * or a stable checkpoint.
     */
    void _recoverFromStableTimestamp(OperationContext* opCtx,
                                     Timestamp stableTimestamp,
                                     OpTime appliedThrough,
                                     OpTime topOfOplog);

    /**
     * After truncating the oplog, completes recovery if we're recovering from an unstable
     * checkpoint.
     */
    void _recoverFromUnstableCheckpoint(OperationContext* opCtx,
                                        OpTime appliedThrough,
                                        OpTime topOfOplog);

    /**
     * Applies all oplog entries from oplogApplicationStartPoint (exclusive) to topOfOplog
     * (inclusive). This fasserts if oplogApplicationStartPoint is not in the oplog.
     */
    void _applyToEndOfOplog(OperationContext* opCtx,
                            const Timestamp& oplogApplicationStartPoint,
                            const Timestamp& topOfOplog);

    /**
     * Gets the last applied OpTime from the end of the oplog. Returns CollectionIsEmpty if there is
     * no oplog.
     */
    StatusWith<OpTime> _getTopOfOplog(OperationContext* opCtx) const;

    /**
     * Truncates the oplog after the "truncateTimestamp" entry. Includes the "truncateTimestamp"
     * entry if "inclusive" is set to true.
     */
    void _truncateOplogTo(OperationContext* opCtx, Timestamp truncateTimestamp, bool inclusive);

    /**
     * Uses the oplogTruncateAfterPoint, accessed via '_consistencyMarkers', to decide whether to
     * truncate part of the oplog. If oplogTruncateAfterPoint has been set, then there may be holes
     * in the oplog after that point. In that case, we will truncate the oplog entries starting at
     * and including the entry associated with the oplogTruncateAfterPoint timestamp.
     *
     * If the oplogTruncateAfterPoint is earlier in time than or equal to the stable timestamp, we
     * will truncate the oplog after the stable timestamp instead. There cannot be holes before a
     * stable timestamp. The oplogTruncateAfterPoint can lag behind the stable timestamp because the
     * oplogTruncateAfterPoint is updated on primaries by an asynchronously looping thread that can
     * potentially be starved.
     */
    void _truncateOplogIfNeededAndThenClearOplogTruncateAfterPoint(
        OperationContext* opCtx, boost::optional<Timestamp> stableTimestamp);

    StorageInterface* _storageInterface;
    ReplicationConsistencyMarkers* _consistencyMarkers;
};

}  // namespace repl
}  // namespace mongo
