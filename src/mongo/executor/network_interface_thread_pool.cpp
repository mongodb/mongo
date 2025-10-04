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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/executor/network_interface_thread_pool.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor


namespace mongo {
namespace executor {

NetworkInterfaceThreadPool::NetworkInterfaceThreadPool(NetworkInterface* net) : _net(net) {}

NetworkInterfaceThreadPool::~NetworkInterfaceThreadPool() {
    try {
        _dtorImpl();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

void NetworkInterfaceThreadPool::_dtorImpl() {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        if (_tasks.empty())
            return;

        _inShutdown = true;
    }

    join();

    invariant(_tasks.empty());
}

void NetworkInterfaceThreadPool::startup() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_started) {
        LOGV2_FATAL(34358, "Attempting to start pool, but it has already started");
    }
    _started = true;

    _consumeTasks(std::move(lk));
}

void NetworkInterfaceThreadPool::shutdown() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }

    _net->signalWorkAvailable();
}

void NetworkInterfaceThreadPool::join() {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        if (_joining) {
            LOGV2_FATAL(34357, "Attempted to join pool more than once");
        }

        _joining = true;
        _started = true;

        if (_consumeState == ConsumeState::kNeutral)
            _consumeTasksInline(std::move(lk));
    }

    _net->signalWorkAvailable();

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _joiningCondition.wait(
        lk, [&] { return _tasks.empty() && (_consumeState == ConsumeState::kNeutral); });
}

void NetworkInterfaceThreadPool::schedule(Task task) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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
void NetworkInterfaceThreadPool::_consumeTasks(stdx::unique_lock<stdx::mutex> lk) {
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
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        if (_consumeState != ConsumeState::kScheduled)
            return;
        _consumeTasksInline(std::move(lk));
    });
    invariant(ret.isOK() || ErrorCodes::isShutdownError(ret.code()));
}

void NetworkInterfaceThreadPool::_consumeTasksInline(stdx::unique_lock<stdx::mutex> lk) {
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
