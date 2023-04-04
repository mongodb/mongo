/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/checkpointer.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

const auto getCheckpointer = ServiceContext::declareDecoration<std::unique_ptr<Checkpointer>>();

MONGO_FAIL_POINT_DEFINE(pauseCheckpointThread);

}  // namespace

Checkpointer* Checkpointer::get(ServiceContext* serviceCtx) {
    return getCheckpointer(serviceCtx).get();
}

Checkpointer* Checkpointer::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void Checkpointer::set(ServiceContext* serviceCtx, std::unique_ptr<Checkpointer> newCheckpointer) {
    auto& checkpointer = getCheckpointer(serviceCtx);
    if (checkpointer) {
        invariant(!checkpointer->running(),
                  "Tried to reset the Checkpointer without shutting down the original instance.");
    }
    checkpointer = std::move(newCheckpointer);
}

void Checkpointer::run() {
    ThreadClient tc(name(), getGlobalServiceContext());
    LOGV2_DEBUG(22307, 1, "Starting thread", "threadName"_attr = name());

    while (true) {
        auto opCtx = tc->makeOperationContext();

        {
            stdx::unique_lock<Latch> lock(_mutex);
            MONGO_IDLE_THREAD_BLOCK;

            // Wait for 'storageGlobalParams.checkpointDelaySecs' seconds; or until either shutdown
            // is signaled or a checkpoint is triggered.
            _sleepCV.wait_for(lock,
                              stdx::chrono::seconds(static_cast<std::int64_t>(
                                  storageGlobalParams.checkpointDelaySecs)),
                              [&] { return _shuttingDown || _triggerCheckpoint; });

            // If the checkpointDelaySecs is set to 0, that means we should skip checkpointing.
            // However, checkpointDelaySecs is adjustable by a runtime server parameter, so we
            // need to wake up to check periodically. The wakeup to check period is arbitrary.
            while (storageGlobalParams.checkpointDelaySecs == 0 && !_shuttingDown &&
                   !_triggerCheckpoint) {
                _sleepCV.wait_for(lock, stdx::chrono::seconds(static_cast<std::int64_t>(3)), [&] {
                    return _shuttingDown || _triggerCheckpoint;
                });
            }

            if (_shuttingDown) {
                invariant(!_shutdownReason.isOK());
                LOGV2_DEBUG(22309,
                            1,
                            "Stopping thread",
                            "threadName"_attr = name(),
                            "reason"_attr = _shutdownReason);
                return;
            }

            // Clear the trigger so we do not immediately checkpoint again after this.
            _triggerCheckpoint = false;
        }

        pauseCheckpointThread.pauseWhileSet();

        const Date_t startTime = Date_t::now();
        opCtx->getServiceContext()->getStorageEngine()->checkpoint(opCtx.get());

        const auto secondsElapsed = durationCount<Seconds>(Date_t::now() - startTime);
        if (secondsElapsed >= 30) {
            LOGV2_DEBUG(22308,
                        1,
                        "Checkpoint was slow to complete",
                        "secondsElapsed"_attr = secondsElapsed);
        }
    }
}

void Checkpointer::triggerFirstStableCheckpoint(Timestamp prevStable,
                                                Timestamp initialData,
                                                Timestamp currStable) {
    stdx::unique_lock<Latch> lock(_mutex);
    invariant(!_hasTriggeredFirstStableCheckpoint);
    if (prevStable < initialData && currStable >= initialData) {
        LOGV2(22310,
              "Triggering the first stable checkpoint",
              "initialDataTimestamp"_attr = initialData,
              "prevStableTimestamp"_attr = prevStable,
              "currStableTimestamp"_attr = currStable);
        _hasTriggeredFirstStableCheckpoint = true;
        _triggerCheckpoint = true;
        _sleepCV.notify_one();
    }
}

bool Checkpointer::hasTriggeredFirstStableCheckpoint() {
    stdx::unique_lock<Latch> lock(_mutex);
    return _hasTriggeredFirstStableCheckpoint;
}

void Checkpointer::shutdown(const Status& reason) {
    LOGV2(22322, "Shutting down checkpoint thread");

    {
        stdx::unique_lock<Latch> lock(_mutex);
        _shuttingDown = true;
        _shutdownReason = reason;

        // Wake up the checkpoint thread early, to take a final checkpoint before shutting down, if
        // one has not coincidentally just been taken.
        _sleepCV.notify_one();
    }

    wait();
    LOGV2(22323, "Finished shutting down checkpoint thread");
}

}  // namespace mongo
