/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

class FixedIntervalPolicy final : public CheckpointSchedulePolicy {
public:
    explicit FixedIntervalPolicy(ClockSource* clock) : _clock(clock) {}

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
        // periodically. The wakeup to check period is arbitrary.
        while (storageGlobalParams.syncdelay.load() == 0 && !shouldWake()) {
            _clock->waitForConditionFor(cv, lock, Seconds(3), [&] { return shouldWake(); });
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
