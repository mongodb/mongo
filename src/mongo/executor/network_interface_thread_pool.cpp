// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/executor/network_interface_thread_pool.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/observable_mutex_registry.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

NetworkInterfaceThreadPool::NetworkInterfaceThreadPool(NetworkInterface* net) : _net(net) {
    ObservableMutexRegistry::get().add("networkInterfaceThreadPoolMutex", _mutex);
}

NetworkInterfaceThreadPool::~NetworkInterfaceThreadPool() {
    try {
        _dtorImpl();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

void NetworkInterfaceThreadPool::_dtorImpl() {
    {
        std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);

        if (_tasks.empty())
            return;

        _inShutdown = true;
    }

    join();

    invariant(_tasks.empty());
}

void NetworkInterfaceThreadPool::startup() {
    std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);
    if (_started) {
        LOGV2_FATAL(34358, "Attempting to start pool, but it has already started");
    }
    _started = true;

    _consumeTasks(std::move(lk));
}

void NetworkInterfaceThreadPool::shutdown() {
    {
        std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);
        _inShutdown = true;
    }

    _net->signalWorkAvailable();
}

void NetworkInterfaceThreadPool::join() {
    {
        std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);

        if (_joining) {
            LOGV2_FATAL(34357, "Attempted to join pool more than once");
        }

        _joining = true;
        _started = true;

        if (_consumeState == ConsumeState::kNeutral)
            _consumeTasksInline(std::move(lk));
    }

    _net->signalWorkAvailable();

    std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);
    _joiningCondition.wait(
        lk, [&] { return _tasks.empty() && (_consumeState == ConsumeState::kNeutral); });
}

void NetworkInterfaceThreadPool::schedule(Task task) {
    std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);
    if (_inShutdown) {
        lk.unlock();
        task({ErrorCodes::ShutdownInProgress, "Shutdown in progress"});
        return;
    }

    _tasks.emplace_back(std::move(task));

    if (_started)
        _consumeTasks(std::move(lk));
}

/**
 * Consumes available tasks.
 *
 * We distinguish between calls to consume on the networking thread and off of it. For off thread
 * calls, we try to initiate a consume via schedule and invoke directly inside the executor. This
 * allows us to use the network interface's threads as our own pool, which should reduce context
 * switches if our tasks are getting scheduled by network interface tasks.
 */
void NetworkInterfaceThreadPool::_consumeTasks(std::unique_lock<ObservableMutex<std::mutex>> lk) {
    if ((_consumeState != ConsumeState::kNeutral) || _tasks.empty())
        return;

    auto shouldNotSchedule = _inShutdown || _net->onNetworkThread();
    if (shouldNotSchedule) {
        _consumeTasksInline(std::move(lk));
        return;
    }

    _consumeState = ConsumeState::kScheduled;
    lk.unlock();
    auto ret = _net->schedule([this](Status status) {
        std::unique_lock<ObservableMutex<std::mutex>> lk(_mutex);

        if (_consumeState != ConsumeState::kScheduled)
            return;
        _consumeTasksInline(std::move(lk));
    });
    invariant(ret.isOK() || ErrorCodes::isShutdownError(ret.code()));
}

void NetworkInterfaceThreadPool::_consumeTasksInline(
    std::unique_lock<ObservableMutex<std::mutex>> lk) {
    _consumeState = ConsumeState::kConsuming;
    const ScopeGuard consumingTasksGuard([&] { _consumeState = ConsumeState::kNeutral; });

    decltype(_tasks) tasks;

    while (_tasks.size()) {
        using std::swap;
        swap(tasks, _tasks);

        lk.unlock();
        const ScopeGuard lkGuard([&] { lk.lock(); });

        for (auto&& task : tasks) {
            task(Status::OK());
        }

        tasks.clear();
    }

    if (_joining)
        _joiningCondition.notify_one();
}

}  // namespace executor
}  // namespace mongo
