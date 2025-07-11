/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

SessionScheduler::~SessionScheduler() {
    {
        // Technically here we don't need to hold the mutex, however the same variable is checked
        // in combination with _tasks.empty(), so although rare, it is possible that we might access
        // _tasks while setting the stop variable. This should not happen. Without the mutex, a
        // thread could wake up prematurely and see _tasks.empty() before _stop.load() is visible.
        // This could result in missed wakeups.
        stdx::unique_lock<stdx::mutex> lock(_queueMutex);
        _stop.store(true);  // Indicate shutdown
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
        stdx::unique_lock<stdx::mutex> lock(_queueMutex);
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
        stdx::unique_lock<stdx::mutex> lock(_errorMutex);
        _errors.push_back(err);
    }
}

std::vector<std::exception_ptr> SessionScheduler::getExecutionErrors() {
    stdx::unique_lock<stdx::mutex> lock(_errorMutex);
    return _errors;
}
