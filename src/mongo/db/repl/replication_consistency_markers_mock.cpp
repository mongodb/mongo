// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/replication_consistency_markers_mock.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <mutex>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

void ReplicationConsistencyMarkersMock::initializeMinValidDocument(OperationContext* opCtx) {
    {
        std::lock_guard<std::mutex> lock(_initialSyncFlagMutex);
        _initialSyncFlag = false;
    }

    {
        std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
        _minValid = {};
        _oplogTruncateAfterPoint = {};
        _appliedThrough = {};
    }
}

bool ReplicationConsistencyMarkersMock::getInitialSyncFlag(OperationContext* opCtx) const {
    std::lock_guard<std::mutex> lock(_initialSyncFlagMutex);
    return _initialSyncFlag;
}

void ReplicationConsistencyMarkersMock::setInitialSyncFlag(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_initialSyncFlagMutex);
    _initialSyncFlag = true;
}

void ReplicationConsistencyMarkersMock::clearInitialSyncFlag(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_initialSyncFlagMutex);
    _initialSyncFlag = false;
}

void ReplicationConsistencyMarkersMock::ensureFastCountOnOplogTruncateAfterPoint(
    OperationContext* opCtx) {}

void ReplicationConsistencyMarkersMock::setOplogTruncateAfterPoint(OperationContext* opCtx,
                                                                   const Timestamp& timestamp) {
    std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
    _oplogTruncateAfterPoint = timestamp;
}

Timestamp ReplicationConsistencyMarkersMock::getOplogTruncateAfterPoint(
    OperationContext* opCtx) const {
    std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
    return _oplogTruncateAfterPoint;
}

void ReplicationConsistencyMarkersMock::startUsingOplogTruncateAfterPointForPrimary() {}

void ReplicationConsistencyMarkersMock::stopUsingOplogTruncateAfterPointForPrimary() {}

bool ReplicationConsistencyMarkersMock::isOplogTruncateAfterPointBeingUsedForPrimary() const {
    return true;
}

void ReplicationConsistencyMarkersMock::setOplogTruncateAfterPointToTopOfOplog(
    OperationContext* opCtx) {};

boost::optional<OpTimeAndWallTime>
ReplicationConsistencyMarkersMock::refreshOplogTruncateAfterPointIfPrimary(
    OperationContext* opCtx) {
    return boost::none;
}

void ReplicationConsistencyMarkersMock::setAppliedThrough(OperationContext* opCtx,
                                                          const OpTime& optime) {
    invariant(!optime.isNull());
    std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
    _appliedThrough = optime;
}

void ReplicationConsistencyMarkersMock::clearAppliedThrough(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
    _appliedThrough = {};
}

OpTime ReplicationConsistencyMarkersMock::getAppliedThrough(OperationContext* opCtx) const {
    if (getAppliedThroughFn) {
        getAppliedThroughFn(opCtx);
    }
    std::lock_guard<std::mutex> lock(_minValidBoundariesMutex);
    return _appliedThrough;
}

Status ReplicationConsistencyMarkersMock::createInternalCollections(OperationContext* opCtx) {
    return Status::OK();
}

void ReplicationConsistencyMarkersMock::setInitialSyncIdIfNotSet(OperationContext* opCtx) {
    if (_initialSyncId.isEmpty()) {
        _initialSyncId = UUID::gen().toBSON();
    }
}

void ReplicationConsistencyMarkersMock::clearInitialSyncId(OperationContext* opCtx) {
    _initialSyncId = BSONObj();
}

BSONObj ReplicationConsistencyMarkersMock::getInitialSyncId(OperationContext* opCtx) {
    return _initialSyncId;
}

void ReplicationConsistencyMarkersMock::createRecordIdsGroupIdCollection(OperationContext* opCtx) {}

void ReplicationConsistencyMarkersMock::setRecordIdsGroupId(OperationContext* opCtx,
                                                            const UUID& groupId,
                                                            const OpTime& foundingOpTime) {
    RecordIdsGroupIdDocument doc;
    doc.set_id(kRecordIdsGroupIdDocId);
    doc.setGroupId(groupId);
    doc.setFoundingOpTime(foundingOpTime);

    std::lock_guard<std::mutex> lock(_recordIdsGroupIdMutex);
    // A groupId is only ever written while groupless, and writing over an existing one is always
    // a bug.
    invariant(!_recordIdsGroupId.has_value(),
              "setRecordIdsGroupId called while a recordIdsGroupId document is already present");
    _recordIdsGroupId = doc;
}

boost::optional<RecordIdsGroupIdDocument> ReplicationConsistencyMarkersMock::getRecordIdsGroupId(
    OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_recordIdsGroupIdMutex);
    return _recordIdsGroupId;
}

void ReplicationConsistencyMarkersMock::clearRecordIdsGroupId(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_recordIdsGroupIdMutex);
    _recordIdsGroupId = boost::none;
}

}  // namespace repl
}  // namespace mongo
