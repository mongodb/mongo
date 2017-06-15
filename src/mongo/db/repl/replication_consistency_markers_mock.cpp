/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_consistency_markers_mock.h"

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
        _oplogDeleteFromPoint = {};
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

OpTime ReplicationConsistencyMarkersMock::getMinValid(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    return _minValid;
}

void ReplicationConsistencyMarkersMock::setMinValid(OperationContext* opCtx,
                                                    const OpTime& minValid) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _minValid = minValid;
}

void ReplicationConsistencyMarkersMock::setMinValidToAtLeast(OperationContext* opCtx,
                                                             const OpTime& minValid) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _minValid = std::max(_minValid, minValid);
}

void ReplicationConsistencyMarkersMock::setOplogDeleteFromPoint(OperationContext* opCtx,
                                                                const Timestamp& timestamp) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _oplogDeleteFromPoint = timestamp;
}

Timestamp ReplicationConsistencyMarkersMock::getOplogDeleteFromPoint(
    OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    return _oplogDeleteFromPoint;
}

void ReplicationConsistencyMarkersMock::setAppliedThrough(OperationContext* opCtx,
                                                          const OpTime& optime) {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    _appliedThrough = optime;
}

OpTime ReplicationConsistencyMarkersMock::getAppliedThrough(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> lock(_minValidBoundariesMutex);
    return _appliedThrough;
}

}  // namespace repl
}  // namespace mongo
