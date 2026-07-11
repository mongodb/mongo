// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {
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
    void _consumeOneTask(std::unique_lock<std::mutex>& lk);
    void _shutdown(std::unique_lock<std::mutex>& lk);
    void _join(std::unique_lock<std::mutex>& lk);

    // These are the options with which the pool was configured at construction time.
    const Options _options;

    std::mutex _mutex;
    stdx::thread _worker;
    std::vector<Task> _tasks;
    PseudoRandom _prng;
    NetworkInterfaceMock* const _net;
    bool _started = false;
    bool _inShutdown = false;
    bool _joining = false;
};

}  // namespace executor
}  // namespace mongo
