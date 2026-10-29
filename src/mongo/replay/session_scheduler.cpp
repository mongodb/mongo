// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/session_scheduler.h"

using namespace mongo;

SessionScheduler::SessionScheduler(size_t size) : _stop(false), _hasRecordedErrors(false) {
    uassert(ErrorCodes::ReplayClientSessionSchedulerError,
            "Error, At least one session consumer thread is needed.",
            size >= 1);

    for (size_t i = 0; i < size; ++i) {
        addWorker();
    }
}

void SessionScheduler::join() {
    {
        // Technically here we don't need to hold the mutex, however the same variable is checked
        // in combination with _tasks.empty(), so although rare, it is possible that we might access
        // _tasks while setting the stop variable. This should not happen. Without the mutex, a
        // thread could wake up prematurely and see _tasks.empty() before _stop.load() is visible.
        // This could result in missed wakeups.
        std::unique_lock<std::mutex> lock(_queueMutex);
        if (_stop.swap(true)) {
            // Stop already set; workers have been notified and joined already.
            return;
        }
    }
    // Notify all sessions to stop
    _condition.notify_all();
    for (auto& worker : _workers) {
        if (worker.joinable()) {
            // Ensure all threads (sessions) are joined before destructing
            worker.join();
        }
    }
}

SessionScheduler::~SessionScheduler() {
    join();
}

void SessionScheduler::addWorker() {
    _workers.emplace_back([this]() {
        bool running = true;
        while (running) {
            running = executeTask();
        }
    });
}

bool SessionScheduler::executeTask() {
    SessionTask task;
    // Queue management with a lock
    {
        std::unique_lock<std::mutex> lock(_queueMutex);
        _condition.wait(lock, [this] { return _stop.load() || !_tasks.empty(); });

        if (_tasks.empty() && _stop.load()) {
            // stop processing, the session has not more work to do
            return false;
        }
        task = std::move(_tasks.front());
        _tasks.pop();
    }

    // Eventual errors must be handled by the caller (SessionSimulator or SessionHandler)
    task();
    return true;
}

void SessionScheduler::recordError(std::exception_ptr err) {
    _hasRecordedErrors.store(true);
    {
        std::unique_lock<std::mutex> lock(_errorMutex);
        _errors.push_back(err);
    }
}

std::vector<std::exception_ptr> SessionScheduler::getExecutionErrors() {
    std::unique_lock<std::mutex> lock(_errorMutex);
    return _errors;
}
