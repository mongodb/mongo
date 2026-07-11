// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/checkpoint_schedule_policy.h"

#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/duration.h"

#include <cstdint>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

// Poll interval used when checkpointing is disabled via syncdelay=0.
constexpr Seconds kSyncdelayDisabledPollInterval{3};

class FixedIntervalPolicy final : public CheckpointSchedulePolicy {
public:
    explicit FixedIntervalPolicy(ClockSource* clock) : _clock(clock) {}

    bool accumulateOplogBytes(int64_t) override {
        return false;
    }

    void waitUntilReady(std::unique_lock<std::mutex>& lock,
                        stdx::condition_variable& cv,
                        std::function<bool()> shouldWake) override {
        MONGO_IDLE_THREAD_BLOCK;

        // Wait for 'storageGlobalParams.syncdelay' seconds; or until either shutdown is
        // signaled or a checkpoint is triggered.
        LOGV2_DEBUG(7702900,
                    1,
                    "Checkpoint thread sleeping",
                    "duration"_attr =
                        static_cast<std::int64_t>(storageGlobalParams.syncdelay.load()));
        _clock->waitForConditionFor(
            cv, lock, Seconds(static_cast<int64_t>(storageGlobalParams.syncdelay.load())), [&] {
                return shouldWake();
            });

        // If the syncdelay is set to 0, that means we should skip checkpointing. However,
        // syncdelay is adjustable by a runtime server parameter, so we need to wake up to check
        // periodically.
        while (storageGlobalParams.syncdelay.load() == 0 && !shouldWake()) {
            _clock->waitForConditionFor(
                cv, lock, kSyncdelayDisabledPollInterval, [&] { return shouldWake(); });
        }
    }

private:
    ClockSource* _clock;
};

}  // namespace

std::unique_ptr<CheckpointSchedulePolicy> createFixedIntervalPolicy(ClockSource* clock) {
    return std::make_unique<FixedIntervalPolicy>(clock);
}

}  // namespace mongo
