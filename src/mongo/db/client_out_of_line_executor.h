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

#pragma once

#include <functional>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/producer_consumer_queue.h"

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

    ~ClientOutOfLineExecutor() noexcept;

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
    AtomicWord<bool> _requireShutdown{false};

    class Impl;
    std::unique_ptr<Impl> _impl;

    std::shared_ptr<QueueType> _taskQueue;

    // Provides the means to ensure `shutdown()` always precedes the destructor.
    bool _isShutdown = false;
};

}  // namespace mongo
