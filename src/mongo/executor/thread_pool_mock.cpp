// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/executor/thread_pool_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"

#include <cstddef>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

ThreadPoolMock::ThreadPoolMock(NetworkInterfaceMock* net, int32_t prngSeed, Options options)
    : _options(std::move(options)), _prng(prngSeed), _net(net) {}

ThreadPoolMock::~ThreadPoolMock() {
    std::unique_lock<std::mutex> lk(_mutex);
    if (_joining)
        return;

    _shutdown(lk);
    _join(lk);
}

void ThreadPoolMock::startup() {
    LOGV2_DEBUG(22602, 1, "Starting pool");
    std::lock_guard<std::mutex> lk(_mutex);
    invariant(!_started);
    invariant(!_worker.joinable());
    _started = true;
    _worker = stdx::thread([this] {
        _options.onCreateThread();
        std::unique_lock<std::mutex> lk(_mutex);

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
    std::unique_lock<std::mutex> lk(_mutex);
    _shutdown(lk);
}

void ThreadPoolMock::join() {
    std::unique_lock<std::mutex> lk(_mutex);
    _join(lk);
}

void ThreadPoolMock::schedule(Task task) {
    std::unique_lock<std::mutex> lk(_mutex);
    if (_inShutdown) {
        lk.unlock();

        task({ErrorCodes::ShutdownInProgress, "Shutdown in progress"});
        return;
    }

    _tasks.emplace_back(std::move(task));
    _net->signalWorkAvailable();
}

void ThreadPoolMock::_consumeOneTask(std::unique_lock<std::mutex>& lk) {
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

void ThreadPoolMock::_shutdown(std::unique_lock<std::mutex>& lk) {
    LOGV2_DEBUG(22605, 1, "Shutting down pool");

    _inShutdown = true;
    _net->signalWorkAvailable();
}

void ThreadPoolMock::_join(std::unique_lock<std::mutex>& lk) {
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
