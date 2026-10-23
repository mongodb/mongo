// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <condition_variable>
#include <deque>
#include <mutex>

#include <boost/optional.hpp>

namespace mongo {

/**
 * A single/multiple-producer queue, and a thread that actively pulls items off the queue in FIFO
 * order and processes them.
 *
 * Worker thread starts at construction and runs until either:
 *  - `SerialProcessor::shutdown()` called
 *  - its "process function" returns an error status
 *  - the processor is destructed
 *
 * Processor function cannot be changed at runtime, and shutdown is idempotent and irreversible.
 *
 * Not copyable or moveable.
 */
template <typename Item>
class [[MONGO_MOD_OPEN]] SerialProcessor {
public:
    static constexpr std::string_view kShutdownReason{"SerialProcessor was requested to shutdown"};

    using ProcessFn = unique_function<Status(Item)>;
    using InitFn = unique_function<Status()>;

    struct Options {
        ProcessFn process = [](Item) {
            return Status::OK();
        };
        InitFn init = [] {
            return Status::OK();
        };
    };

    SerialProcessor() = delete;

    explicit SerialProcessor(Options opts)
        : _process(std::move(opts.process)),
          _init(std::move(opts.init)),
          _worker([this] { _workerLoop(); }) {}

    ~SerialProcessor() {
        shutdown();
    }

    // Pushes an item to the queue, only if the processor is able to accept new items.
    void push(Item item) {
        std::unique_lock lk{_mutex};
        iassert(_status);
        _items.emplace_back(std::move(item));
        if (_items.size() == 1) {
            _cvWork.notify_one();
        }
    }

    // Signals the worker to stop and waits for it to join (see: `shutdownAsync()`).
    // All future calls to `push()` will return an error status with reason
    // `SerialProcessor::kShutdownReason`, unless the worker gave us an error (either before or
    // during its joining the main thread).
    // This action is idempotent and irreversible.
    // Returns the items that were left unprocessed in the queue.
    std::deque<Item> shutdown() {
        shutdownAsync();
        // Guard join() so concurrent shutdown() callers don't double-join the worker.
        // This is safe because after `_worker` is constructed, this lambda is the only place we
        // attempt to access it.
        std::call_once(_joinOnce, [this] {
            invariant(_worker.joinable());
            _worker.join();
        });
        std::unique_lock lk{_mutex};
        return std::move(_items);
    }

    // Signals the worker thread to shut down without waiting for it to join.
    // All future calls to `push()` will return an error status with reason
    // `SerialProcessor::kShutdownReason`, unless the worker gave us an error (either before or
    // during its joining the main thread).
    void shutdownAsync() {
        std::unique_lock lk{_mutex};
        if (_status.isOK()) {
            _status = {ErrorCodes::ShutdownInProgress, kShutdownReason};
        }
        _cvWork.notify_one();
        _cvDrain.notify_all();
    }

    // Returns Status::OK() if the processor can accept new items.
    // Returns status with reason `SerialProcessor::kShutdownReason` if the processor was shut down.
    // If the worker had returned an error, returns the error status from the worker.
    [[nodiscard]] Status getStatus() const {
        std::lock_guard lk{_mutex};
        return _status;
    }

    // Waits until no items are queued or in flight, or the processor has failed/shut down.
    // Returns Status::OK() if the processor can accept new items.
    // Returns status with reason `SerialProcessor::kShutdownReason` if the processor was shut down.
    // If the worker had returned an error, returns the error status from the worker.
    void drain() {
        std::unique_lock lk{_mutex};
        invariant(_drain(lk));
    }

    // Waits `duration` milliseconds until no items are queued or in flight, or the processor has
    // failed/shut down.
    // Returns `true` if the request did not time out.
    bool drain(Milliseconds duration) {
        std::unique_lock lk{_mutex};
        return _drain(lk, duration);
    }

    SerialProcessor(const SerialProcessor&) = delete;
    SerialProcessor& operator=(const SerialProcessor&) = delete;

    SerialProcessor(SerialProcessor&& rhs) = delete;
    SerialProcessor& operator=(SerialProcessor&& rhs) = delete;

private:
    void _workerLoop() {
        std::unique_lock lk{_mutex};
        auto initStatus = _init();
        if (!initStatus.isOK()) {
            _status = initStatus;
            _cvDrain.notify_all();
            return;
        }
        for (;;) {
            _cvWork.wait(lk, [&] { return !_status.isOK() || !_items.empty(); });
            if (MONGO_unlikely(!_status.isOK())) {
                _cvDrain.notify_all();
                return;
            }
            invariant(!_items.empty());
            auto item = std::move(_items.front());
            lk.unlock();
            Status status = _process(std::move(item));
            lk.lock();
            _items.pop_front();
            if (MONGO_unlikely(!status.isOK())) {
                _status = std::move(status);
                _cvDrain.notify_all();
                return;
            }
            if (_items.empty()) {
                _cvDrain.notify_all();
            }
        }
    }

    bool _drain(std::unique_lock<std::mutex>& lk,
                boost::optional<Milliseconds> timeout = boost::none) {
        if (!timeout.has_value()) {
            _cvDrain.wait(lk, [&] { return _items.empty(); });
            return true;
        }

        return _cvDrain.wait_for(lk, timeout->toSystemDuration(), [&] { return _items.empty(); });
    }

    ProcessFn _process;
    InitFn _init;
    mutable std::mutex _mutex;
    std::deque<Item> _items;
    Status _status = Status::OK();
    stdx::condition_variable _cvWork;
    stdx::condition_variable _cvDrain;
    stdx::thread _worker;
    std::once_flag _joinOnce;
};

}  // namespace mongo
