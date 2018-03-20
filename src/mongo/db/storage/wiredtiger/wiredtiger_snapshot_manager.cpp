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

#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"

#include <algorithm>
#include <cstdio>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

void WiredTigerSnapshotManager::setCommittedSnapshot(const Timestamp& timestamp) {
    stdx::lock_guard<stdx::mutex> lock(_committedSnapshotMutex);

    invariant(!_committedSnapshot || *_committedSnapshot <= timestamp);
    _committedSnapshot = timestamp;
}

void WiredTigerSnapshotManager::setLocalSnapshot(const Timestamp& timestamp) {
    stdx::lock_guard<stdx::mutex> lock(_localSnapshotMutex);
    _localSnapshot = timestamp;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getLocalSnapshot() {
    stdx::lock_guard<stdx::mutex> lock(_localSnapshotMutex);
    return _localSnapshot;
}

void WiredTigerSnapshotManager::dropAllSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_committedSnapshotMutex);
    _committedSnapshot = boost::none;
}

boost::optional<Timestamp> WiredTigerSnapshotManager::getMinSnapshotForNextCommittedRead() const {
    stdx::lock_guard<stdx::mutex> lock(_committedSnapshotMutex);
    return _committedSnapshot;
}

Status WiredTigerSnapshotManager::setTransactionReadTimestamp(Timestamp pointInTime,
                                                              WT_SESSION* session,
                                                              bool roundToOldest) const {
    char readTSConfigString[15 /* read_timestamp= */ + 16 /* 16 hexadecimal digits */ +
                            17 /* ,round_to_oldest= */ + 5 /* false */ + 1 /* trailing null */];
    auto size = std::snprintf(readTSConfigString,
                              sizeof(readTSConfigString),
                              "read_timestamp=%llx,round_to_oldest=%s",
                              pointInTime.asULL(),
                              (roundToOldest) ? "true" : "false");
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

    stdx::lock_guard<stdx::mutex> lock(_committedSnapshotMutex);
    uassert(ErrorCodes::ReadConcernMajorityNotAvailableYet,
            "Committed view disappeared while running operation",
            _committedSnapshot);

    auto status = setTransactionReadTimestamp(_committedSnapshot.get(), session);
    fassert(30635, status);
    rollbacker.Dismiss();
    return *_committedSnapshot;
}

void WiredTigerSnapshotManager::beginTransactionOnLocalSnapshot(WT_SESSION* session,
                                                                bool ignorePrepare) const {
    invariantWTOK(
        session->begin_transaction(session, (ignorePrepare) ? "ignore_prepare=true" : nullptr));
    auto rollbacker =
        MakeGuard([&] { invariant(session->rollback_transaction(session, nullptr) == 0); });

    stdx::lock_guard<stdx::mutex> lock(_localSnapshotMutex);
    invariant(_localSnapshot);

    LOG(3) << "begin_transaction on local snapshot " << _localSnapshot.get().toString();
    auto status = setTransactionReadTimestamp(_localSnapshot.get(), session);
    fassert(50775, status);
    rollbacker.Dismiss();
}

void WiredTigerSnapshotManager::beginTransactionOnOplog(WiredTigerOplogManager* oplogManager,
                                                        WT_SESSION* session) const {
    invariantWTOK(session->begin_transaction(session, nullptr));
    auto rollbacker =
        MakeGuard([&] { invariant(session->rollback_transaction(session, nullptr) == 0); });

    auto allCommittedTimestamp = oplogManager->getOplogReadTimestamp();
    invariant(Timestamp(static_cast<unsigned long long>(allCommittedTimestamp)).asULL() ==
              allCommittedTimestamp);
    auto status = setTransactionReadTimestamp(
        Timestamp(static_cast<unsigned long long>(allCommittedTimestamp)),
        session,
        true /* roundToOldest */);

    fassert(50771, status);
    rollbacker.Dismiss();
}

}  // namespace mongo
