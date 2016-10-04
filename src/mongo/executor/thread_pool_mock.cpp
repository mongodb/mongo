/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/thread_pool_mock.h"

#include "mongo/executor/network_interface_mock.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {

ThreadPoolMock::ThreadPoolMock(NetworkInterfaceMock* net, int32_t prngSeed, Options options)
    : _options(std::move(options)), _prng(prngSeed), _net(net) {}

ThreadPoolMock::~ThreadPoolMock() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _inShutdown = true;
    _net->signalWorkAvailable();
    if (_started) {
        if (_worker.joinable()) {
            lk.unlock();
            _worker.join();
            lk.lock();
        }
    } else {
        consumeTasks(&lk);
    }
    invariant(_tasks.empty());
}

void ThreadPoolMock::startup() {
    LOG(1) << "Starting pool";
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_started);
    invariant(!_worker.joinable());
    _started = true;
    _worker = stdx::thread([this] {
        _options.onCreateThread();
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        consumeTasks(&lk);
    });
}

void ThreadPoolMock::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _inShutdown = true;
    _net->signalWorkAvailable();
}

void ThreadPoolMock::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _joining = true;
    if (_started) {
        stdx::thread toJoin = std::move(_worker);
        _net->signalWorkAvailable();
        lk.unlock();
        toJoin.join();
        lk.lock();
        invariant(_tasks.empty());
    } else {
        consumeTasks(&lk);
        invariant(_tasks.empty());
    }
}

Status ThreadPoolMock::schedule(Task task) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_inShutdown) {
        return {ErrorCodes::ShutdownInProgress, "Shutdown in progress"};
    }
    _tasks.emplace_back(std::move(task));
    return Status::OK();
}

void ThreadPoolMock::consumeTasks(stdx::unique_lock<stdx::mutex>* lk) {
    using std::swap;

    LOG(1) << "Starting to consume tasks";
    while (!(_inShutdown && _tasks.empty())) {
        if (_tasks.empty()) {
            lk->unlock();
            _net->waitForWork();
            lk->lock();
            continue;
        }
        auto next = static_cast<size_t>(_prng.nextInt64(static_cast<int64_t>(_tasks.size())));
        if (next + 1 != _tasks.size()) {
            swap(_tasks[next], _tasks.back());
        }
        Task fn = std::move(_tasks.back());
        _tasks.pop_back();
        lk->unlock();
        fn();
        lk->lock();
    }
    LOG(1) << "Done consuming tasks";

    invariant(_tasks.empty());

    while (!_joining) {
        lk->unlock();
        _net->waitForWork();
        lk->lock();
    }

    LOG(1) << "Ready to join";
}

}  // namespace executor
}  // namespace mongo
