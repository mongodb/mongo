/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/periodic_runner_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

PeriodicRunnerImpl::PeriodicRunnerImpl(ServiceContext* svc, ClockSource* clockSource)
    : _svc(svc), _clockSource(clockSource) {}

PeriodicRunnerImpl::~PeriodicRunnerImpl() {
    shutdown();
}

void PeriodicRunnerImpl::scheduleJob(PeriodicJob job) {
    auto impl = std::make_shared<PeriodicJobImpl>(std::move(job), this);

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _jobs.push_back(impl);
        if (_running) {
            impl->run();
        }
    }
}

void PeriodicRunnerImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_running) {
        return;
    }

    _running = true;

    // schedule any jobs that we have
    for (auto job : _jobs) {
        job->run();
    }
}

void PeriodicRunnerImpl::shutdown() {
    std::vector<stdx::thread> threads;
    const auto guard = MakeGuard([&] {
        for (auto& thread : threads) {
            thread.join();
        }
    });

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_running) {
            _running = false;

            for (auto&& job : _jobs) {
                threads.push_back(std::move(job->thread));
            }

            _jobs.clear();

            _condvar.notify_all();
        }
    }
}

PeriodicRunnerImpl::PeriodicJobImpl::PeriodicJobImpl(PeriodicJob job, PeriodicRunnerImpl* parent)
    : job(std::move(job)), parent(parent) {}

void PeriodicRunnerImpl::PeriodicJobImpl::run() {
    thread = stdx::thread([ this, anchor = shared_from_this() ] {
        Client::initThread(job.name, parent->_svc, nullptr);

        while (true) {
            auto start = parent->_clockSource->now();

            job.job(Client::getCurrent());

            stdx::unique_lock<stdx::mutex> lk(parent->_mutex);
            if (parent->_clockSource->waitForConditionUntil(
                    parent->_condvar, lk, start + job.interval, [&] {
                        return !parent->_running;
                    })) {
                break;
            }
        }
    });
}

}  // namespace mongo
