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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Timestamp;

namespace repl {

/**
 * A mock ReplicationConsistencyMarkers implementation that stores everything in memory.
 */
class ReplicationConsistencyMarkersMock : public ReplicationConsistencyMarkers {
    MONGO_DISALLOW_COPYING(ReplicationConsistencyMarkersMock);

public:
    ReplicationConsistencyMarkersMock() = default;

    void initializeMinValidDocument(OperationContext* opCtx) override;

    bool getInitialSyncFlag(OperationContext* opCtx) const override;
    void setInitialSyncFlag(OperationContext* opCtx) override;
    void clearInitialSyncFlag(OperationContext* opCtx) override;

    OpTime getMinValid(OperationContext* opCtx) const override;
    void setMinValid(OperationContext* opCtx, const OpTime& minValid) override;
    void setMinValidToAtLeast(OperationContext* opCtx, const OpTime& minValid) override;

    void setOplogDeleteFromPoint(OperationContext* opCtx, const Timestamp& timestamp) override;
    Timestamp getOplogDeleteFromPoint(OperationContext* opCtx) const override;

    void setAppliedThrough(OperationContext* opCtx, const OpTime& optime) override;
    OpTime getAppliedThrough(OperationContext* opCtx) const override;

private:
    mutable stdx::mutex _initialSyncFlagMutex;
    bool _initialSyncFlag = false;

    mutable stdx::mutex _minValidBoundariesMutex;
    OpTime _appliedThrough;
    OpTime _minValid;
    Timestamp _oplogDeleteFromPoint;
};

}  // namespace repl
}  // namespace mongo
