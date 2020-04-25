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

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/thread_pool_mock.h"

#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace executor {

ThreadPoolMock::ThreadPoolMock(NetworkInterfaceMock* net, int32_t prngSeed, Options options)
    : _options(std::move(options)), _prng(prngSeed), _net(net) {}

ThreadPoolMock::~ThreadPoolMock() {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_joining)
        return;

    _shutdown(lk);
    _join(lk);
}

void ThreadPoolMock::startup() {
    LOGV2_DEBUG(22602, 1, "Starting pool");
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_started);
    invariant(!_worker.joinable());
    _started = true;
    _worker = stdx::thread([this] {
        _options.onCreateThread();
        stdx::unique_lock<Latch> lk(_mutex);

        LOGV2_DEBUG(22603, 1, "Starting to consume tasks");
        while (!_joining) {
            if (_tasks.empty()) {
                lk.unlock();
                _net->waitForWork();
                lk.lock();
                continue;
            }

            _consumeOneTask(lk);
        }
        LOGV2_DEBUG(22604, 1, "Done consuming tasks");
    });
}

void ThreadPoolMock::shutdown() {
    stdx::unique_lock<Latch> lk(_mutex);
    _shutdown(lk);
}

void ThreadPoolMock::join() {
    stdx::unique_lock<Latch> lk(_mutex);
    _join(lk);
}

void ThreadPoolMock::schedule(Task task) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_inShutdown) {
        lk.unlock();

        task({ErrorCodes::ShutdownInProgress, "Shutdown in progress"});
        return;
    }

    _tasks.emplace_back(std::move(task));
}

void ThreadPoolMock::_consumeOneTask(stdx::unique_lock<Latch>& lk) {
    auto next = static_cast<size_t>(_prng.nextInt64(static_cast<int64_t>(_tasks.size())));
    if (next + 1 != _tasks.size()) {
        std::swap(_tasks[next], _tasks.back());
    }
    Task fn = std::move(_tasks.back());
    _tasks.pop_back();
    lk.unlock();
    if (_inShutdown) {
        fn({ErrorCodes::ShutdownInProgress, "Shutdown in progress"});
    } else {
        fn(Status::OK());
    }
    lk.lock();
}

void ThreadPoolMock::_shutdown(stdx::unique_lock<Latch>& lk) {
    LOGV2_DEBUG(22605, 1, "Shutting down pool");

    _inShutdown = true;
    _net->signalWorkAvailable();
}

void ThreadPoolMock::_join(stdx::unique_lock<Latch>& lk) {
    LOGV2_DEBUG(22606, 1, "Joining pool");

    _joining = true;
    _net->signalWorkAvailable();
    _net->exitNetwork();

    // Since there is only one worker thread, we need to consume tasks here to potentially
    // unblock that thread.
    while (!_tasks.empty()) {
        _consumeOneTask(lk);
    }

    if (_started) {
        lk.unlock();
        _worker.join();
        lk.lock();
    }

    invariant(_tasks.empty());
}

}  // namespace executor
}  // namespace mongo
