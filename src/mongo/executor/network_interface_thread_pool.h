// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/out_of_line_executor.h"

#include <cstdint>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>

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
class [[MONGO_MOD_NEEDS_REPLACEMENT]] NetworkInterfaceThreadPool final
    : public ThreadPoolInterface {
public:
    NetworkInterfaceThreadPool(NetworkInterface* net);
    ~NetworkInterfaceThreadPool() override;

    void startup() override;
    void shutdown() override;
    void join() override;
    void schedule(Task task) override;

private:
    void _consumeTasks(std::unique_lock<ObservableMutex<std::mutex>> lk);
    void _consumeTasksInline(std::unique_lock<ObservableMutex<std::mutex>> lk);
    void _dtorImpl();

    NetworkInterface* const _net;

    // Protects all of the pool state below
    ObservableMutex<std::mutex> _mutex;
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
