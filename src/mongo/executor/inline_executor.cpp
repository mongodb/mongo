/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/executor/inline_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/transport/baton.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <list>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::executor {

namespace {
class Scheduler : public OutOfLineExecutor {
public:
    explicit Scheduler(std::shared_ptr<InlineExecutor::State>& state) : _state(state) {}
    ~Scheduler() override = default;

    void schedule(OutOfLineExecutor::Task func) override {
        if (auto state = _state.lock()) {
            try {
                state->tasks.push(std::move(func));
                return;
            } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
                // The corresponding `InlineExecutor` can no longer accept work.
            }
        }

        func({ErrorCodes::ShutdownInProgress, "The instance of InlineExecutor is shutdown!"});
    }

private:
    std::weak_ptr<InlineExecutor::State> _state;
};
}  // namespace

InlineExecutor::InlineExecutor()
    : _state(std::make_shared<State>()), _executor(std::make_shared<Scheduler>(_state)) {}

InlineExecutor::~InlineExecutor() {
    _state->tasks.closeProducerEnd();
    ON_BLOCK_EXIT([&] { _state->tasks.closeConsumerEnd(); });

    try {
        while (auto maybeTask = _state->tasks.tryPop()) {
            (*maybeTask)({ErrorCodes::ShutdownInProgress, "Stopping the inline executor"});
        }
    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueConsumed>&) {
        // Nothing to do as this is the result of calling `tryPop` on an empty queue.
    }
}

void InlineExecutor::run(std::function<bool()> predicate, Interruptible* interruptible) {
    while (!predicate()) {
        _state->tasks.pop(interruptible)(Status::OK());
    }
}

namespace {
class SleepableExecutorImpl : public InlineExecutor::SleepableExecutor {
public:
    using Sleeper = std::function<SemiFuture<void>(Milliseconds, const CancellationToken&)>;

    SleepableExecutorImpl() = delete;
    SleepableExecutorImpl(std::shared_ptr<OutOfLineExecutor> executor, Sleeper sleeper)
        : _executor(std::move(executor)), _sleeper(std::move(sleeper)) {}

    void schedule(OutOfLineExecutor::Task task) override {
        _executor->schedule(std::move(task));
    }

    ExecutorFuture<void> sleepFor(Milliseconds duration, const CancellationToken& token) override {
        return _sleeper(duration, token).thenRunOn(_executor);
    }

private:
    std::shared_ptr<OutOfLineExecutor> _executor;
    Sleeper _sleeper;
};
}  // namespace

std::shared_ptr<InlineExecutor::SleepableExecutor> InlineExecutor::getSleepableExecutor(
    const std::shared_ptr<TaskExecutor>& executor, const std::shared_ptr<Baton>& baton) {
    SleepableExecutorImpl::Sleeper sleeper;
    if (baton && baton->networking()) {
        sleeper = [baton](Milliseconds duration, const CancellationToken& token) {
            return baton->networking()->waitUntil(Date_t::now() + duration, token).semi();
        };
    } else {
        invariant(executor);
        sleeper = [executor](Milliseconds duration, const CancellationToken& token) {
            return executor->sleepFor(duration, token).semi();
        };
    }
    return std::make_shared<SleepableExecutorImpl>(getExecutor(), std::move(sleeper));
}

}  // namespace mongo::executor
