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

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_clock_source.h"

#include <functional>
#include <memory>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Abstract interface governing when the Checkpointer thread wakes up to take a checkpoint.
 * The Checkpointer calls waitUntilReady() each loop iteration and then takes a checkpoint.
 */
class [[MONGO_MOD_OPEN]] CheckpointSchedulePolicy {
public:
    virtual ~CheckpointSchedulePolicy() = default;

    /**
     * Blocks until the next checkpoint should be taken. shouldWake() returns true when the
     * Checkpointer has been asked to shut down or take an immediate checkpoint.
     */
    virtual void waitUntilReady(std::unique_lock<std::mutex>& lock,
                                stdx::condition_variable& cv,
                                std::function<bool()> shouldWake) = 0;
};

/**
 * Returns a CheckpointSchedulePolicy that wakes on the fixed interval governed by
 * storageGlobalParams.syncdelay.
 */
std::unique_ptr<CheckpointSchedulePolicy> createFixedIntervalPolicy(
    ClockSource* clock = SystemClockSource::get());

}  // namespace mongo
