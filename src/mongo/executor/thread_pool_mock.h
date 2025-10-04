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

#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace MONGO_MOD_PUB mongo {
namespace executor {

class NetworkInterfaceMock;

/**
 * Implementation of a thread pool that is tightly integrated with NetworkInterfaceMock to allow for
 * deterministic unit testing of ThreadPoolTaskExecutor. This pool has a single thread, which only
 * executes jobs when the NetworkInterfaceMock allows it to (i.e., not when the NetworkInterfaceMock
 * is in the "enterNetwork" mode.
 */
class ThreadPoolMock final : public ThreadPoolInterface {
public:
    /**
     * Structure used to configure an instance of ThreadPoolMock.
     */
    struct Options {
        // This function is run before the worker thread begins consuming tasks.
        using OnCreateThreadFn = std::function<void()>;
        OnCreateThreadFn onCreateThread = []() {
        };
    };

    /**
     * Create an instance that interlocks with "net". "prngSeed" seeds the pseudorandom number
     * generator that is used to determine which schedulable task runs next.
     */
    ThreadPoolMock(NetworkInterfaceMock* net, int32_t prngSeed, Options options);
    ~ThreadPoolMock() override;

    void startup() override;
    void shutdown() override;
    void join() override;
    void schedule(Task task) override;

private:
    void _consumeOneTask(stdx::unique_lock<stdx::mutex>& lk);
    void _shutdown(stdx::unique_lock<stdx::mutex>& lk);
    void _join(stdx::unique_lock<stdx::mutex>& lk);

    // These are the options with which the pool was configured at construction time.
    const Options _options;

    stdx::mutex _mutex;
    stdx::thread _worker;
    std::vector<Task> _tasks;
    PseudoRandom _prng;
    NetworkInterfaceMock* const _net;
    bool _started = false;
    bool _inShutdown = false;
    bool _joining = false;
};

}  // namespace executor
}  // namespace MONGO_MOD_PUB mongo
