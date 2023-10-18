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


#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <mutex>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/sync_source_feedback.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {


const Milliseconds maximumKeepAliveIntervalMS(30 * 1000);

// The network timeout used for replSetUpdatePosition requests made to a node's sync source.
const Seconds syncSourceFeedbackNetworkTimeoutSecs(30);

/**
 * Calculates the keep alive interval based on the given ReplSetConfig.
 */
Milliseconds calculateKeepAliveInterval(const ReplSetConfig& rsConfig) {
    return std::min((rsConfig.getElectionTimeoutPeriod() / 2), maximumKeepAliveIntervalMS);
}

/**
 * Returns function to prepare update command
 */
Reporter::PrepareReplSetUpdatePositionCommandFn makePrepareReplSetUpdatePositionCommandFn(
    ReplicationCoordinator* replCoord, const HostAndPort& syncTarget, BackgroundSync* bgsync) {
    return [syncTarget, replCoord, bgsync]() -> StatusWith<BSONObj> {
        auto currentSyncTarget = bgsync->getSyncTarget();
        if (currentSyncTarget != syncTarget) {
            if (currentSyncTarget.empty()) {
                // Sync source was cleared.
                return Status(ErrorCodes::InvalidSyncSource,
                              str::stream() << "Sync source was cleared. Was " << syncTarget);

            } else {
                // Sync source changed.
                return Status(ErrorCodes::InvalidSyncSource,
                              str::stream() << "Sync source changed from " << syncTarget << " to "
                                            << currentSyncTarget);
            }
        }

        if (replCoord->getMemberState().primary()) {
            // Primary has no one to send updates to.
            return Status(ErrorCodes::InvalidSyncSource,
                          "Currently primary - no one to send updates to");
        }

        return replCoord->prepareReplSetUpdatePositionCommand();
    };
}

}  // namespace

void SyncSourceFeedback::forwardSecondaryProgress() {
    {
        stdx::unique_lock<Latch> lock(_mtx);
        _positionChanged = true;
        _cond.notify_all();
        if (_reporter) {
            auto triggerStatus = _reporter->trigger();
            if (!triggerStatus.isOK()) {
                LOGV2_WARNING(21764,
                              "Unable to forward progress",
                              "target"_attr = _reporter->getTarget(),
                              "error"_attr = triggerStatus);
            }
        }
    }
}

Status SyncSourceFeedback::_updateUpstream(Reporter* reporter) {
    auto syncTarget = reporter->getTarget();

    auto triggerStatus = reporter->trigger();
    if (!triggerStatus.isOK()) {
        LOGV2_WARNING(21765,
                      "Unable to schedule reporter to update replication progress",
                      "syncTarget"_attr = syncTarget,
                      "error"_attr = triggerStatus);
        return triggerStatus;
    }

    auto status = reporter->join();

    if (!status.isOK()) {
        LOGV2(21760,
              "SyncSourceFeedback error sending update",
              "syncTarget"_attr = syncTarget,
              "error"_attr = status);
    }

    // Sync source denylisting will be done in BackgroundSync and SyncSourceResolver.

    return status;
}

void SyncSourceFeedback::shutdown() {
    stdx::unique_lock<Latch> lock(_mtx);
    if (_reporter) {
        _reporter->shutdown();
    }
    _shutdownSignaled = true;
    _cond.notify_all();
}

void SyncSourceFeedback::run(executor::TaskExecutor* executor,
                             BackgroundSync* bgsync,
                             ReplicationCoordinator* replCoord) {
    Client::initThread("SyncSourceFeedback",
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer));

    HostAndPort syncTarget;

    // keepAliveInterval indicates how frequently to forward progress in the absence of updates.
    Milliseconds keepAliveInterval(0);

    while (true) {  // breaks once _shutdownSignaled is true

        if (keepAliveInterval == Milliseconds(0)) {
            keepAliveInterval = calculateKeepAliveInterval(replCoord->getConfig());
        }

        {
            // Take SyncSourceFeedback lock before calling into ReplicationCoordinator
            // to avoid deadlock because ReplicationCoordinator could conceivably calling back into
            // this class.
            stdx::unique_lock<Latch> lock(_mtx);
            while (!_positionChanged && !_shutdownSignaled) {
                {
                    MONGO_IDLE_THREAD_BLOCK;
                    if (_cond.wait_for(lock, keepAliveInterval.toSystemDuration()) !=
                        stdx::cv_status::timeout) {
                        continue;
                    }
                }
                MemberState state = replCoord->getMemberState();
                if (!(state.primary() || state.startup())) {
                    break;
                }
            }

            if (_shutdownSignaled) {
                break;
            }

            _positionChanged = false;
        }

        {
            stdx::lock_guard<Latch> lock(_mtx);
            MemberState state = replCoord->getMemberState();
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
            LOGV2_DEBUG(21761, 1, "Setting syncSourceFeedback", "target"_attr = target);
            syncTarget = target;

            // Update keepalive value from config.
            auto oldKeepAliveInterval = keepAliveInterval;
            keepAliveInterval = calculateKeepAliveInterval(replCoord->getConfig());
            if (oldKeepAliveInterval != keepAliveInterval) {
                LOGV2_DEBUG(21762,
                            1,
                            "New syncSourceFeedback keep alive duration",
                            "newKeepAliveInterval"_attr = keepAliveInterval,
                            "oldKeepAliveInterval"_attr = oldKeepAliveInterval);
            }
        }

        Reporter reporter(executor,
                          makePrepareReplSetUpdatePositionCommandFn(replCoord, syncTarget, bgsync),
                          syncTarget,
                          keepAliveInterval,
                          syncSourceFeedbackNetworkTimeoutSecs);
        {
            stdx::lock_guard<Latch> lock(_mtx);
            if (_shutdownSignaled) {
                break;
            }
            _reporter = &reporter;
        }
        ON_BLOCK_EXIT([this]() {
            stdx::lock_guard<Latch> lock(_mtx);
            _reporter = nullptr;
        });

        auto status = _updateUpstream(&reporter);
        if (!status.isOK()) {
            LOGV2_DEBUG(
                21763,
                1,
                "The replication progress command (replSetUpdatePosition) failed and will be "
                "retried",
                "error"_attr = status);
        }
    }
}

}  // namespace repl
}  // namespace mongo
