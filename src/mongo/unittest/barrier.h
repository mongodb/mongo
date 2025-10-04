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

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mongo {
namespace unittest {

/**
 * Holds internal thread counter that is set on initialization.
 * This counter is decremented every time a thread enters countDownAndWait() and blocks.
 * All threads are unblocked when the counter reaches zero and the counter is reset.
 */
class Barrier {
    Barrier(const Barrier&) = delete;
    Barrier& operator=(const Barrier&) = delete;

public:
    /**
     * Initializes barrier with a default thread count.
     */
    explicit Barrier(size_t threadCount);

    /**
     * Decrements thread counter by 1. If the thread counter is not zero, the thread blocks
     * until the counter reaches zero.
     */
    void countDownAndWait();

private:
    size_t _threadCount;
    size_t _threadsWaiting;
    uint64_t _generation;
    stdx::mutex _mutex;
    stdx::condition_variable _condition;
};

}  // namespace unittest
}  // namespace mongo
