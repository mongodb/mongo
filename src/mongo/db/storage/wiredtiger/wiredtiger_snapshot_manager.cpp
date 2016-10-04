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

#include "mongo/base/checked_cast.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_snapshot_manager.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

Status WiredTigerSnapshotManager::prepareForCreateSnapshot(OperationContext* txn) {
    WiredTigerRecoveryUnit::get(txn)->prepareForCreateSnapshot(txn);
    return Status::OK();
}

Status WiredTigerSnapshotManager::createSnapshot(OperationContext* txn, const SnapshotName& name) {
    auto session = WiredTigerRecoveryUnit::get(txn)->getSession(txn)->getSession();
    const std::string config = str::stream() << "name=" << name.asU64();
    return wtRCToStatus(session->snapshot(session, config.c_str()));
}

void WiredTigerSnapshotManager::setCommittedSnapshot(const SnapshotName& name) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    invariant(!_committedSnapshot || *_committedSnapshot <= name);
    _committedSnapshot = name;
}

void WiredTigerSnapshotManager::cleanupUnneededSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_committedSnapshot)
        return;

    const std::string config = str::stream() << "drop=(before=" << _committedSnapshot->asU64()
                                             << ')';
    invariantWTOK(_session->snapshot(_session, config.c_str()));
}

void WiredTigerSnapshotManager::dropAllSnapshots() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _committedSnapshot = boost::none;
    invariantWTOK(_session->snapshot(_session, "drop=(all)"));
}

void WiredTigerSnapshotManager::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_session)
        return;
    invariantWTOK(_session->close(_session, NULL));
    _session = nullptr;
}

boost::optional<SnapshotName> WiredTigerSnapshotManager::getMinSnapshotForNextCommittedRead()
    const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _committedSnapshot;
}

SnapshotName WiredTigerSnapshotManager::beginTransactionOnCommittedSnapshot(
    WT_SESSION* session) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    uassert(ErrorCodes::ReadConcernMajorityNotAvailableYet,
            "Committed view disappeared while running operation",
            _committedSnapshot);

    StringBuilder config;
    config << "snapshot=" << _committedSnapshot->asU64();
    invariantWTOK(session->begin_transaction(session, config.str().c_str()));

    return *_committedSnapshot;
}

}  // namespace mongo
