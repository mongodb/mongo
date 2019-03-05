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

#pragma once

#include <cstdint>
#include <vector>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool_interface.h"

namespace mongo {
namespace executor {

class NetworkInterface;

/**
 * A Thread Pool implementation based on running tasks on the background thread
 * of some Network Interface.
 *
 * The basic idea is to triage tasks, running them immediately if we're invoked
 * from on the network interface thread, and queueing them up to be drained by
 * a setAlarm if not.
 */
class NetworkInterfaceThreadPool final : public ThreadPoolInterface {
public:
    NetworkInterfaceThreadPool(NetworkInterface* net);
    ~NetworkInterfaceThreadPool() override;

    void startup() override;
    void shutdown() override;
    void join() override;
    void schedule(Task task) override;

private:
    void _consumeTasks(stdx::unique_lock<stdx::mutex> lk);
    void _consumeTasksInline(stdx::unique_lock<stdx::mutex> lk) noexcept;
    void _dtorImpl();

    NetworkInterface* const _net;

    // Protects all of the pool state below
    stdx::mutex _mutex;
    stdx::condition_variable _joiningCondition;
    std::vector<Task> _tasks;
    bool _started = false;
    bool _inShutdown = false;
    bool _joining = false;

    enum class ConsumeState {
        kNeutral = 0,
        kScheduled,
        kConsuming,
    };
    ConsumeState _consumeState = ConsumeState::kNeutral;
};

}  // namespace executor
}  // namespace mongo
