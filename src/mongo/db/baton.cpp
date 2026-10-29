// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/baton.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/functional.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mongo {

namespace {

/**
 * The sub baton proxies requests to the underlying baton until it is detached.  After that point,
 * all jobs within the sub baton will be failed with a ShutdownInProgress status and all further
 * work will be refused.
 *
 * This type does not fail outstanding networking work on detach and should be used with a
 * ScopedTaskExecutor if task executor level task failure is desired.
 */
class SubBaton final : public Baton {
    static const inline auto kDetached =
        Status(ErrorCodes::ShutdownInProgress, "SubBaton detached");

public:
    explicit SubBaton(BatonHandle baton) : _baton(std::move(baton)) {}

    ~SubBaton() override {
        std::lock_guard lk(_mutex);
        invariant(_isDead);
    }

    void schedule(Task func) override {
        {
            std::unique_lock lk(_mutex);

            if (_isDead) {
                lk.unlock();
                func(kDetached);
                return;
            }

            _scheduled.emplace_back(std::move(func));

            // if we have more than 1 element, we previously called schedule
            if (_scheduled.size() > 1) {
                return;
            }
        }

        _baton->schedule([this, anchor = shared_from_this()](Status status) {
            _runJobs(std::unique_lock<std::mutex>(_mutex), status);
        });
    }

    transport::NetworkingBaton* networking() noexcept override {
        return _baton->networking();
    }

    void run(ClockSource* clkSource) noexcept override {
        {
            std::lock_guard lk(_mutex);
            invariant(!_isDead);
        }

        _baton->run(clkSource);
    }

    TimeoutState run_until(ClockSource* clkSource, Date_t deadline) noexcept override {
        {
            std::lock_guard lk(_mutex);
            invariant(!_isDead);
        }

        return _baton->run_until(clkSource, deadline);
    }

    void notify() noexcept override {
        if (std::lock_guard lk(_mutex); _isDead) {
            return;
        }

        _baton->notify();
    }

    void detachImpl() noexcept override {
        std::unique_lock<std::mutex> lk(_mutex);
        _isDead = true;

        _runJobs(std::move(lk), kDetached);
    }

    Future<void> waitUntil(Date_t expiration, const CancellationToken& token) noexcept override {
        if (std::lock_guard lk(_mutex); _isDead) {
            return kDetached;
        }

        return _baton->waitUntil(expiration, token);
    }

private:
    void _runJobs(std::unique_lock<std::mutex> lk, Status status) {
        if (status.isOK() && _isDead) {
            status = kDetached;
        }

        auto toRun = std::exchange(_scheduled, {});

        lk.unlock();
        // There's always a race in between checking if we're detached and calling a
        // callback.  This dispatch doesn't change that (and allows for the invocation of
        // callbacks with Status::OK, even if one of them internally detaches the baton, if
        // they were part of the same batch).
        for (auto& job : toRun) {
            job(status);
        }
    }

    BatonHandle _baton;

    std::mutex _mutex;
    bool _isDead = false;
    std::vector<Task> _scheduled;
};

}  // namespace

auto Baton::makeSubBaton() -> SubBatonHolder {
    return SubBatonHolder(std::make_shared<SubBaton>(shared_from_this()));
}

}  // namespace mongo
