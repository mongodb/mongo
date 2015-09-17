/**
*    Copyright (C) 2008-2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/db/repl/snapshot_thread.h"

#include "mongo/platform/basic.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::endl;
using std::string;
using std::stringstream;

namespace repl {
MONGO_FP_DECLARE(disableSnapshotting);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(replSnapshotThreadThrottleMicros, int, 1000);

SnapshotThread::SnapshotThread(SnapshotManager* manager)
    : _manager(manager), _thread([this] { run(); }) {}

void SnapshotThread::run() {
    Client::initThread("SnapshotThread");
    auto& client = cc();
    auto serviceContext = client.getServiceContext();
    auto replCoord = ReplicationCoordinator::get(serviceContext);

    Timestamp lastTimestamp = {};
    while (true) {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_inShutdown)
                return;
        }
        {
            // This block logically belongs at the end of the loop, but having it at the top
            // simplifies handling of the "continue" cases. It is harmless to do these before the
            // first run of the loop.
            _manager->cleanupUnneededSnapshots();
            sleepmicros(replSnapshotThreadThrottleMicros);  // Throttle by sleeping.
        }

        {
            std::shared_ptr<CappedInsertNotifier> notifier;
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            if (!_notifier || _notifier->isDead()) {
                lk.unlock();
                auto txn = client.makeOperationContext();
                AutoGetCollectionForRead oplog(txn.get(), rsOplogName);

                if (!oplog.getCollection()) {
                    sleepmillis(200);
                    continue;
                }

                lk.lock();
                _notifier = notifier = oplog.getCollection()->getCappedInsertNotifier();
                invariant(notifier);
                lk.unlock();
            } else {
                notifier = _notifier;
                lk.unlock();
            }

            while (true) {
                auto currentTimestamp = getLastSetTimestamp();

                stdx::unique_lock<stdx::mutex> lock(_mutex);
                if (_inShutdown)
                    return;

                if (_forcedSnapshotPending || lastTimestamp != currentTimestamp) {
                    _forcedSnapshotPending = false;
                    lastTimestamp = currentTimestamp;
                    break;
                }

                if (notifier->isDead()) {
                    notifier.reset();
                    _notifier.reset();
                    break;
                }

                lock.unlock();
                notifier->wait();
            }

            // might need to re-acquire the notifier
            if (!notifier)
                continue;
        }

        while (MONGO_FAIL_POINT(disableSnapshotting)) {
            sleepsecs(1);
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            if (_inShutdown) {
                return;
            }
        }

        try {
            auto txn = client.makeOperationContext();
            Lock::GlobalLock globalLock(txn->lockState(), MODE_IS, UINT_MAX);

            if (!replCoord->getMemberState().readable()) {
                // If our MemberState isn't readable, we may not be in a consistent state so don't
                // take snapshots. When we transition into a readable state from a non-readable
                // state, a snapshot is forced to ensure we don't miss the latest write. This must
                // be checked each time we acquire the global IS lock since that prevents the node
                // from transitioning to a !readable() state from a readable() one in the cases
                // where we shouldn't be creating a snapshot.
                continue;
            }

            SnapshotName name(0);  // assigned real value in block.
            {
                // Make sure there are no in-flight capped inserts while we create our snapshot.
                Lock::ResourceLock cappedInsertLockForOtherDb(
                    txn->lockState(), resourceCappedInFlightForOtherDb, MODE_X);
                Lock::ResourceLock cappedInsertLockForLocalDb(
                    txn->lockState(), resourceCappedInFlightForLocalDb, MODE_X);

                // Reserve the name immediately before we take our snapshot. This ensures that all
                // names that compare lower must be from points in time visible to this named
                // snapshot.
                name = replCoord->reserveSnapshotName(nullptr);

                // This establishes the view that we will name.
                _manager->prepareForCreateSnapshot(txn.get());
            }

            auto opTimeOfSnapshot = OpTime();
            {
                AutoGetCollectionForRead oplog(txn.get(), rsOplogName);
                invariant(oplog.getCollection());
                // Read the latest op from the oplog.
                auto cursor = oplog.getCollection()->getCursor(txn.get(), /*forward*/ false);
                auto record = cursor->next();
                if (!record)
                    continue;  // oplog is completely empty.

                const auto op = record->data.releaseToBson();
                opTimeOfSnapshot = fassertStatusOK(28780, OpTime::parseFromOplogEntry(op));
                invariant(!opTimeOfSnapshot.isNull());
            }

            _manager->createSnapshot(txn.get(), name);
            replCoord->onSnapshotCreate(opTimeOfSnapshot, name);
        } catch (const WriteConflictException& wce) {
            log() << "skipping storage snapshot pass due to write conflict";
            continue;
        }
    }
}

void SnapshotThread::shutdown() {
    invariant(_thread.joinable());
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(!_inShutdown);
        _inShutdown = true;
        if (_notifier) {
            _notifier->notifyAll();
        }
    }
    _thread.join();
}

void SnapshotThread::forceSnapshot() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _forcedSnapshotPending = true;
    if (_notifier) {
        _notifier->notifyAll();
    }
}

std::unique_ptr<SnapshotThread> SnapshotThread::start(ServiceContext* service) {
    if (auto manager = service->getGlobalStorageEngine()->getSnapshotManager()) {
        return std::unique_ptr<SnapshotThread>(new SnapshotThread(manager));
    }
    return {};
}

}  // namespace repl
}  // namespace mongo
