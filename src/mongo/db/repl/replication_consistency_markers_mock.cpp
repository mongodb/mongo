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
        stdx::lock_guard<stdx::mutex> lock(_initialSyncFlagMutex);
        _initialSyncFlag = false;
    }

    {
        stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
        _minValid = {};
        _oplogTruncateAfterPoint = {};
        _appliedThrough = {};
    }
}

bool ReplicationConsistencyMarkersMock::getInitialSyncFlag(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_initialSyncFlagMutex);
    return _initialSyncFlag;
}

void ReplicationConsistencyMarkersMock::setInitialSyncFlag(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_initialSyncFlagMutex);
    _initialSyncFlag = true;
}

void ReplicationConsistencyMarkersMock::clearInitialSyncFlag(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_initialSyncFlagMutex);
    _initialSyncFlag = false;
}

void ReplicationConsistencyMarkersMock::ensureFastCountOnOplogTruncateAfterPoint(
    OperationContext* opCtx) {}

void ReplicationConsistencyMarkersMock::setOplogTruncateAfterPoint(OperationContext* opCtx,
                                                                   const Timestamp& timestamp) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _oplogTruncateAfterPoint = timestamp;
}

Timestamp ReplicationConsistencyMarkersMock::getOplogTruncateAfterPoint(
    OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
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
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _appliedThrough = optime;
}

void ReplicationConsistencyMarkersMock::clearAppliedThrough(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _appliedThrough = {};
}

OpTime ReplicationConsistencyMarkersMock::getAppliedThrough(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
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

}  // namespace repl
}  // namespace mongo
