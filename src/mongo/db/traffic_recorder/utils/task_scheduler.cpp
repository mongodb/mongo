/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/traffic_recorder/utils/task_scheduler.h"

#include "mongo/db/query/util/stop_token.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/time_support.h"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace mongo {

namespace detail {
// Differs from std/boost impl in that it allows taking ownership of the popped value.
// std/boost provide only const access via top().
template <class Value, class Comparator = std::less<>>
class priority_queue {
public:
    bool empty() const {
        return _data.empty();
    }

    void push(Value v) {
        _data.push_back(std::move(v));
        std::push_heap(_data.begin(), _data.end(), _comp);
    }

    const Value& top() const {
        return _data.at(0);
    }

    Value take_top() {
        std::pop_heap(_data.begin(), _data.end(), _comp);
        auto result = std::move(_data.back());
        _data.pop_back();
        return result;
    }

    void clear() {
        _data.clear();
    }

private:
    std::vector<Value> _data;
    Comparator _comp;
};
}  // namespace detail

class TaskSchedulerImpl : public TaskScheduler {
public:
    TaskSchedulerImpl(std::string name) {
        _worker = stdx::thread([this, token = _stop.get_token(), name = std::move(name)]() {
            setThreadName(std::move(name));
            _doWork(token);
        });
    }

    void runAt(Date_t timepoint, unique_function<void()> work) override {
        {
            auto ul = std::unique_lock(_mutex);
            _tasks.push({timepoint, std::move(work)});
        }
        _cv.notify_one();
    }

    void cancelAll() override {
        {
            auto ul = std::unique_lock(_mutex);
            _tasks.clear();
        }
        _cv.notify_one();
    }

    void stop() override {
        _stop.request_stop();
        _cv.notify_one();
        if (_worker.joinable()) {
            _worker.join();
        }
        stopped.store(true);
    }

    ~TaskSchedulerImpl() override {
        if (!stopped.load()) {
            stop();
        }
    }

private:
    void _doWork(stop_token token) {
        auto ul = std::unique_lock(_mutex);
        while (!token.stop_requested()) {
            _cv.wait(ul, [&] { return token.stop_requested() || !_tasks.empty(); });
            if (_tasks.empty()) {
                // No work; check the stop token or wait again.
                continue;
            }
            const auto& [timepoint, work] = _tasks.top();
            auto now = Date_t::now();

            if (timepoint > now) {
                auto expectedTimepoint = timepoint;
                _cv.wait_for(ul, (timepoint - now).toSystemDuration(), [&] {
                    return token.stop_requested() || _tasks.empty() ||
                        std::get<0>(_tasks.top()) != expectedTimepoint;
                });
                // We woke - either:
                // * Stop has been requested, no further work should be done
                // * It is time to run the task observed previously
                // * A _new_ task with an earlier time has been enqueued
                // * Tasks have been cancelled
                //
                // Check the stop token and the top of the queue again.
                continue;
            }

            // Take ownership of the top piece of work; the queue may be modified
            // while unlocked.
            auto currentTask = _tasks.take_top().second;


            ul.unlock();
            // There is a task, and it is time to run it.
            currentTask();
            ul.lock();
        }
    }

    std::atomic<bool> stopped = false;  // NOLINT

    stop_source _stop;
    stdx::thread _worker;

    std::mutex _mutex;
    std::condition_variable _cv;  // NOLINT

    struct TimeOrdered {
        bool operator()(const auto& a, const auto& b) {
            // Ignore the task itself, just sort by time.
            // The _smaller_ element should be top, so
            // sort greater than, rather than less than.
            return std::get<0>(a) > std::get<0>(b);
        }
    };

    using QueueElement = std::pair<Date_t, unique_function<void()>>;
    detail::priority_queue<QueueElement, TimeOrdered> _tasks;
};

std::unique_ptr<TaskScheduler> makeTaskScheduler(std::string name) {
    return std::make_unique<TaskSchedulerImpl>(std::move(name));
}
}  // namespace mongo
