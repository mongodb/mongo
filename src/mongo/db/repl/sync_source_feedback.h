/**
*    Copyright (C) 2013 10gen Inc.
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
#include "mongo/base/status.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
struct HostAndPort;
class OperationContext;

namespace executor {
class TaskExecutor;
}  // namespace executor

namespace repl {
class BackgroundSync;
class Reporter;

class SyncSourceFeedback {
    MONGO_DISALLOW_COPYING(SyncSourceFeedback);

public:
    SyncSourceFeedback() = default;

    /// Notifies the SyncSourceFeedbackThread to wake up and send an update upstream of slave
    /// replication progress.
    void forwardSlaveProgress();

    /**
     * Loops continuously until shutdown() is called, passing updates when they are present. If no
     * update occurs within the _keepAliveInterval, progress is forwarded to let the upstream node
     * know that this node, along with the alive nodes chaining through it, are still alive.
     *
     * Task executor is used to run replSetUpdatePosition command on sync source.
     */
    void run(executor::TaskExecutor* executor, BackgroundSync* bgsync);

    /// Signals the run() method to terminate.
    void shutdown();

private:
    /* Inform the sync target of our current position in the oplog, as well as the positions
     * of all secondaries chained through us.
     */
    Status _updateUpstream(OperationContext* txn, BackgroundSync* bgsync);

    // protects cond, _shutdownSignaled, _keepAliveInterval, and _positionChanged.
    stdx::mutex _mtx;
    // used to alert our thread of changes which need to be passed up the chain
    stdx::condition_variable _cond;
    // used to indicate a position change which has not yet been pushed along
    bool _positionChanged = false;
    // Once this is set to true the _run method will terminate
    bool _shutdownSignaled = false;
    // Reports replication progress to sync source.
    Reporter* _reporter = nullptr;
};

}  // namespace repl
}  // namespace mongo
