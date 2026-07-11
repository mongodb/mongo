// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/background.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {
class Checkpointer final : public BackgroundJob {
public:
    explicit Checkpointer(std::unique_ptr<CheckpointSchedulePolicy> policy)
        : BackgroundJob(false /* deleteSelf */),
          _shuttingDown(false),
          _shutdownReason(Status::OK()),
          _hasTriggeredFirstStableCheckpoint(false),
          _triggerCheckpoint(false),
          _policy(std::move(policy)) {}

    static Checkpointer* get(ServiceContext* serviceCtx);
    static Checkpointer* get(OperationContext* opCtx);
    static void set(ServiceContext* serviceCtx, std::unique_ptr<Checkpointer> newCheckpointer);

    std::string name() const override {
        return "Checkpointer";
    }

    /**
     * Starts the checkpoint thread. Wake intervals are governed by the injected
     * CheckpointSchedulePolicy.
     */
    void run() override;

    /**
     * Triggers taking the first stable checkpoint if the stable timestamp has advanced past the
     * initial data timestamp.
     *
     * The checkpoint thread wakes at intervals governed by the injected policy. This function
     * avoids potentially waiting that full duration for a stable checkpoint, initiating
     * one immediately.
     *
     * Do not call this function if hasTriggeredFirstStableCheckpoint() returns true.
     */
    void triggerFirstStableCheckpoint(Timestamp prevStable,
                                      Timestamp initialData,
                                      Timestamp currStable);

    /**
     * Returns whether the first stable checkpoint has already been triggered.
     */
    bool hasTriggeredFirstStableCheckpoint();

    /**
     * Blocks until the checkpoint thread has been fully shutdown.
     */
    void shutdown(const Status& reason);

    /**
     * Notifies the checkpointer that oplog bytes have been written. The policy accumulates the
     * bytes and wakes the checkpoint thread early when the volume threshold is first crossed in
     * a cycle.
     */
    void notifyOplogWrite(int64_t bytes);

    /**
     * Blocks the checkpointer thread from running until the first checkpoint has been explicitly
     * taken on step-up.
     */
    void pauseCheckpointing();

    /**
     * Once the first checkpoint has been taken, re-allow checkpointer thread activity.
     */
    void resumeCheckpointing();

    /**
     * Returns whether a pauseCheckpointing() request is currently in effect.
     */
    bool isPauseRequested();

private:
    // Protects the state below.
    std::mutex _mutex;

    // The checkpoint thread idles on this condition variable for a particular time duration between
    // taking checkpoints. It can be triggered early to expedite either: immediate checkpointing if
    // _triggerCheckpoint is set; or shutdown cleanup if _shuttingDown is set.
    stdx::condition_variable _sleepCV;

    bool _shuttingDown;
    Status _shutdownReason;

    // This flag ensures the first stable checkpoint is only triggered once.
    bool _hasTriggeredFirstStableCheckpoint;

    // This flag allows the checkpoint thread to wake up early when _sleepCV is signaled.
    bool _triggerCheckpoint;

    stdx::condition_variable _waitForPauseCV;

    // These flags signal that we are waiting for the first checkpoint to be explicitly taken on
    // first step-up.
    bool _isPaused{false};

    bool _pauseRequested{false};

    // Policy that governs when the checkpoint thread wakes to take a checkpoint.
    std::unique_ptr<CheckpointSchedulePolicy> _policy;
};

}  // namespace mongo
