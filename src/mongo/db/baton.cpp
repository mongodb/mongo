/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/baton.h"

#include "mongo/base/status.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"

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

    ~SubBaton() {
        invariant(_isDead);
    }

    void schedule(Task func) noexcept override {
        {
            stdx::unique_lock lk(_mutex);

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
            _runJobs(stdx::unique_lock(_mutex), status);
        });
    }

    transport::NetworkingBaton* networking() noexcept override {
        return _baton->networking();
    }

    void markKillOnClientDisconnect() noexcept override {
        MONGO_UNREACHABLE;
    }

    void run(ClockSource* clkSource) noexcept override {
        invariant(!_isDead);

        _baton->run(clkSource);
    }

    TimeoutState run_until(ClockSource* clkSource, Date_t deadline) noexcept override {
        invariant(!_isDead);

        return _baton->run_until(clkSource, deadline);
    }

    void notify() noexcept override {
        if (stdx::lock_guard lk(_mutex); _isDead) {
            return;
        }

        _baton->notify();
    }

    void detachImpl() noexcept override {
        stdx::unique_lock lk(_mutex);
        _isDead = true;

        _runJobs(std::move(lk), kDetached);
    }

private:
    void _runJobs(stdx::unique_lock<stdx::mutex> lk, Status status) {
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

    stdx::mutex _mutex;
    bool _isDead = false;
    std::vector<Task> _scheduled;
};

}  // namespace

auto Baton::makeSubBaton() -> SubBatonHolder {
    return SubBatonHolder(std::make_shared<SubBaton>(shared_from_this()));
}

}  // namespace mongo
