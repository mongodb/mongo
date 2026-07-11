// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/producer_consumer_queue.h"

#include <functional>
#include <memory>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A client decoration that allows out-of-line, client-local execution of tasks.
 * So long as the client thread exists, tasks can be scheduled to execute on the client thread.
 * Other threads must always schedule tasks through an instance of `QueueHandle`.
 * Once the decoration is destructed, `QueueHandle` runs tasks on the caller's thread, providing
 * `ErrorCodes::CallbackCanceled` as the status.
 */
class ClientOutOfLineExecutor final : public OutOfLineExecutor {
public:
    ClientOutOfLineExecutor() noexcept;

    ~ClientOutOfLineExecutor() override;

    static ClientOutOfLineExecutor* get(const Client*) noexcept;

    using Task = OutOfLineExecutor::Task;

    void shutdown();

    void schedule(Task) override;

    // Blocks until the executor is done running all scheduled tasks.
    void consumeAllTasks() noexcept;

    using QueueType = MultiProducerSingleConsumerQueue<Task>;

    // Allows other threads to access the queue irrespective of the client's lifetime.
    class QueueHandle final {
    public:
        QueueHandle() = default;

        QueueHandle(const std::shared_ptr<QueueType>& queue) : _weakQueue(queue) {}

        void schedule(Task&&);

    private:
        std::weak_ptr<QueueType> _weakQueue;
    };

    auto getHandle() noexcept {
        // We always acquire a handle before scheduling tasks on the executor. The following ensures
        // that acquiring a handle, thus exposing the executor to the outside world, always requires
        // shutdown. A combination of load/store is preferred over using CAS due to performance
        // considerations, and considering the fact that overwriting `_requireShutdown` is safe.
        if (MONGO_unlikely(!_requireShutdown.load()))
            _requireShutdown.store(true);
        return QueueHandle(_taskQueue);
    }

private:
    // Shutdown is not required so long as the executor is not exposed to the outside world.
    Atomic<bool> _requireShutdown{false};

    class Impl;
    std::unique_ptr<Impl> _impl;

    std::shared_ptr<QueueType> _taskQueue;

    // Provides the means to ensure `shutdown()` always precedes the destructor.
    bool _isShutdown = false;
};

}  // namespace mongo
