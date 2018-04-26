/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/base/checked_cast.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

Status WiredTigerSnapshotManager::prepareForCreateSnapshot(OperationContext* opCtx) {
    WiredTigerRecoveryUnit::get(opCtx)->prepareForCreateSnapshot(opCtx);
    return Status::OK();
}

void WiredTigerSnapshotManager::setCommittedSnapshot(const Timestamp& timestamp) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    invariant(!_committedSnapshot || *_committedSnapshot <= timestamp);
    _committedSnapshot = timestamp;
}

void WiredTigerSnapshotManager::cleanupUnneededSnapshots() {}

void WiredTigerSnapshotManager::dropAllSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _committedSnapshot = boost::none;
}

void WiredTigerSnapshotManager::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_session)
        return;
    invariantWTOK(_session->close(_session, NULL));
    _session = nullptr;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getMinSnapshotForNextCommittedRead() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _committedSnapshot;
}

Status WiredTigerSnapshotManager::setTransactionReadTimestamp(Timestamp pointInTime,
                                                              WT_SESSION* session) const {
    char readTSConfigString[15 /* read_timestamp= */ + 16 /* 16 hexadecimal digits */ +
                            1 /* trailing null */];
    auto size = std::snprintf(
        readTSConfigString, sizeof(readTSConfigString), "read_timestamp=%llx", pointInTime.asULL());
    if (size < 0) {
        int e = errno;
        error() << "error snprintf " << errnoWithDescription(e);
        fassertFailedNoTrace(40664);
    }
    invariant(static_cast<std::size_t>(size) < sizeof(readTSConfigString));

    return wtRCToStatus(session->timestamp_transaction(session, readTSConfigString));
}

Timestamp WiredTigerSnapshotManager::beginTransactionOnCommittedSnapshot(
    WT_SESSION* session) const {
    invariantWTOK(session->begin_transaction(session, nullptr));
    auto rollbacker =
        MakeGuard([&] { invariant(session->rollback_transaction(session, nullptr) == 0); });

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    uassert(ErrorCodes::ReadConcernMajorityNotAvailableYet,
            "Committed view disappeared while running operation",
            _committedSnapshot);

    auto status = setTransactionReadTimestamp(_committedSnapshot.get(), session);
    fassertStatusOK(30635, status);
    rollbacker.Dismiss();
    return *_committedSnapshot;
}

void WiredTigerSnapshotManager::beginTransactionOnOplog(WiredTigerOplogManager* oplogManager,
                                                        WT_SESSION* session) const {
    invariantWTOK(session->begin_transaction(session, nullptr));
    auto rollbacker =
        MakeGuard([&] { invariant(session->rollback_transaction(session, nullptr) == 0); });

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto allCommittedTimestamp = oplogManager->getOplogReadTimestamp();
    invariant(Timestamp(static_cast<unsigned long long>(allCommittedTimestamp)).asULL() ==
              allCommittedTimestamp);
    auto status = setTransactionReadTimestamp(
        Timestamp(static_cast<unsigned long long>(allCommittedTimestamp)), session);

    // If we failed to set the read timestamp, we assume it is due to the oldest_timestamp racing
    // ahead.  Rather than synchronizing for this rare case, if requested, throw a
    // WriteConflictException which will be retried.
    if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
        throw WriteConflictException();
    }
    fassert(50771, status);
    rollbacker.Dismiss();
}

}  // namespace mongo
