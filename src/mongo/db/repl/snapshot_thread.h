/*
*    Copyright (C) 2015 MongoDB Inc.
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
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"

namespace mongo {
namespace repl {

/**
 * The thread that makes storage snapshots periodically to enable majority committed reads.
 *
 * Currently the implementation must live in oplog.cpp because it uses newOpMutex.
 * TODO find a better home for this.
 */
class SnapshotThread {
    MONGO_DISALLOW_COPYING(SnapshotThread);

public:
    /**
     * Starts a thread to take periodic snapshots if supported by the storageEngine.
     *
     * If the current storage engine doesn't support snapshots, a null pointer will be returned.
     */
    static std::unique_ptr<SnapshotThread> start(ServiceContext* service);

    /**
     * Signals the thread to stop and waits for it to finish.
     * This must be called exactly once before exitCleanly() takes the global X lock.
     */
    void shutdown();

    /**
     * Forces a new snapshot to be taken even if the global timestamp hasn't changed.
     *
     * Does not wait for the snapshot to be taken.
     */
    void forceSnapshot();

private:
    explicit SnapshotThread(SnapshotManager* manager);
    void run();
    bool shouldSleepMore(int numSleepsDone, size_t numUncommittedSnapshots);

    SnapshotManager* const _manager;
    bool _hitSnapshotLimit = false;

    AtomicWord<bool> _inShutdown{false};             // writes guarded by newOpMutex in oplog.cpp.
    AtomicWord<bool> _forcedSnapshotPending{false};  // writes guarded by newOpMutex in oplog.cpp.
    stdx::thread _thread;
};

}  // namespace repl
}  // namespace mongo
