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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/sync_source_feedback.h"

#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Calculates the keep alive interval based on the current configuration in the replication
 * coordinator.
 */
Milliseconds calculateKeepAliveInterval(OperationContext* txn, stdx::mutex& mtx) {
    stdx::lock_guard<stdx::mutex> lock(mtx);
    auto replCoord = repl::ReplicationCoordinator::get(txn);
    auto rsConfig = replCoord->getConfig();
    auto keepAliveInterval = rsConfig.getElectionTimeoutPeriod() / 2;
    return keepAliveInterval;
}

/**
 * Returns function to prepare update command
 */
Reporter::PrepareReplSetUpdatePositionCommandFn makePrepareReplSetUpdatePositionCommandFn(
    OperationContext* txn,
    stdx::mutex& mtx,
    const HostAndPort& syncTarget,
    BackgroundSync* bgsync) {
    return [&mtx, syncTarget, txn, bgsync](ReplicationCoordinator::ReplSetUpdatePositionCommandStyle
                                               commandStyle) -> StatusWith<BSONObj> {
        auto currentSyncTarget = bgsync->getSyncTarget();
        if (currentSyncTarget != syncTarget) {
            // Change in sync target
            return Status(ErrorCodes::InvalidSyncSource, "Sync target is no longer valid");
        }

        stdx::lock_guard<stdx::mutex> lock(mtx);
        auto replCoord = repl::ReplicationCoordinator::get(txn);
        if (replCoord->getMemberState().primary()) {
            // Primary has no one to send updates to.
            return Status(ErrorCodes::InvalidSyncSource,
                          "Currently primary - no one to send updates to");
        }

        return replCoord->prepareReplSetUpdatePositionCommand(commandStyle);
    };
}

}  // namespace

void SyncSourceFeedback::forwardSlaveProgress() {
    {
        stdx::unique_lock<stdx::mutex> lock(_mtx);
        _positionChanged = true;
        _cond.notify_all();
        if (_reporter) {
            auto triggerStatus = _reporter->trigger();
            if (!triggerStatus.isOK()) {
                warning() << "unable to forward slave progress to " << _reporter->getTarget()
                          << ": " << triggerStatus;
            }
        }
    }
}

Status SyncSourceFeedback::_updateUpstream(OperationContext* txn, BackgroundSync* bgsync) {
    Reporter* reporter;
    {
        stdx::lock_guard<stdx::mutex> lock(_mtx);
        reporter = _reporter;
    }

    auto syncTarget = reporter->getTarget();

    auto triggerStatus = reporter->trigger();
    if (!triggerStatus.isOK()) {
        warning() << "unable to schedule reporter to update replication progress on " << syncTarget
                  << ": " << triggerStatus;
        return triggerStatus;
    }

    auto status = reporter->join();

    if (!status.isOK()) {
        log() << "SyncSourceFeedback error sending update to " << syncTarget << ": " << status;

        // Some errors should not cause result in blacklisting the sync source.
        if (status != ErrorCodes::InvalidSyncSource) {
            // The command could not be created because the node is now primary.
        } else if (status != ErrorCodes::NodeNotFound) {
            // The command could not be created, likely because this node was removed from the set.
        } else {
            // Blacklist sync target for .5 seconds and find a new one.
            stdx::lock_guard<stdx::mutex> lock(_mtx);
            auto replCoord = repl::ReplicationCoordinator::get(txn);
            replCoord->blacklistSyncSource(syncTarget, Date_t::now() + Milliseconds(500));
            bgsync->clearSyncTarget();
        }
    }

    return status;
}

void SyncSourceFeedback::shutdown() {
    stdx::unique_lock<stdx::mutex> lock(_mtx);
    if (_reporter) {
        _reporter->shutdown();
    }
    _shutdownSignaled = true;
    _cond.notify_all();
}

void SyncSourceFeedback::run(executor::TaskExecutor* executor, BackgroundSync* bgsync) {
    Client::initThread("SyncSourceFeedback");

    HostAndPort syncTarget;

    // keepAliveInterval indicates how frequently to forward progress in the absence of updates.
    Milliseconds keepAliveInterval(0);

    while (true) {  // breaks once _shutdownSignaled is true
        auto txn = cc().makeOperationContext();

        if (keepAliveInterval == Milliseconds(0)) {
            keepAliveInterval = calculateKeepAliveInterval(txn.get(), _mtx);
        }

        {
            // Take SyncSourceFeedback lock before calling into ReplicationCoordinator
            // to avoid deadlock because ReplicationCoordinator could conceivably calling back into
            // this class.
            stdx::unique_lock<stdx::mutex> lock(_mtx);
            while (!_positionChanged && !_shutdownSignaled) {
                if (_cond.wait_for(lock, keepAliveInterval.toSystemDuration()) ==
                    stdx::cv_status::timeout) {
                    MemberState state = ReplicationCoordinator::get(txn.get())->getMemberState();
                    if (!(state.primary() || state.startup())) {
                        break;
                    }
                }
            }

            if (_shutdownSignaled) {
                break;
            }

            _positionChanged = false;
        }

        {
            stdx::lock_guard<stdx::mutex> lock(_mtx);
            MemberState state = ReplicationCoordinator::get(txn.get())->getMemberState();
            if (state.primary() || state.startup()) {
                continue;
            }
        }

        const HostAndPort target = bgsync->getSyncTarget();
        // Log sync source changes.
        if (target.empty()) {
            if (syncTarget != target) {
                syncTarget = target;
            }
            // Loop back around again; the keepalive functionality will cause us to retry
            continue;
        }

        if (syncTarget != target) {
            LOG(1) << "setting syncSourceFeedback to " << target;
            syncTarget = target;

            // Update keepalive value from config.
            auto oldKeepAliveInterval = keepAliveInterval;
            keepAliveInterval = calculateKeepAliveInterval(txn.get(), _mtx);
            if (oldKeepAliveInterval != keepAliveInterval) {
                LOG(1) << "new syncSourceFeedback keep alive duration = " << keepAliveInterval
                       << " (previously " << oldKeepAliveInterval << ")";
            }
        }

        Reporter reporter(
            executor,
            makePrepareReplSetUpdatePositionCommandFn(txn.get(), _mtx, syncTarget, bgsync),
            syncTarget,
            keepAliveInterval);
        {
            stdx::lock_guard<stdx::mutex> lock(_mtx);
            _reporter = &reporter;
        }
        ON_BLOCK_EXIT([this]() {
            stdx::lock_guard<stdx::mutex> lock(_mtx);
            _reporter = nullptr;
        });

        auto status = _updateUpstream(txn.get(), bgsync);
        if (!status.isOK()) {
            LOG(1) << "The replication progress command (replSetUpdatePosition) failed and will be "
                      "retried: "
                   << status;
        }
    }
}

}  // namespace repl
}  // namespace mongo
