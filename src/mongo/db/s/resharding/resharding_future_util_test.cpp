/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_future_util.h"

#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_impl.h"

#include <memory>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace {
class ReshardingFutureUtilTest : public unittest::Test {
protected:
    void setUp() override {
        _executor = std::make_shared<ThreadPool>([]() {
            ThreadPool::Options options;
            options.maxThreads = 2;
            return options;
        }());
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    std::shared_ptr<ThreadPool> getExecutor() const {
        return _executor;
    }

private:
    std::shared_ptr<ThreadPool> _executor;
};

TEST_F(ReshardingFutureUtilTest, CancelWhenAnyErrorThenQuiesceDuringExecutorShutdown) {
    CancellationSource cancelSource;
    auto token = cancelSource.token();
    PromiseAndFuture<void> taskThreadsReady;
    AtomicWord<int> tasksRunningCount{0};
    AtomicWord<bool> taskWasCancelled{false};
    auto checkSignalReady = [&]() {
        auto running = tasksRunningCount.addAndFetch(1);
        if (running == 2) {
            taskThreadsReady.promise.emplaceValue();
        }
    };
    PromiseAndFuture<void> executorShutDownTriggered;
    auto quiesced = ExecutorFuture(getExecutor()).then([&]() {
        return resharding::cancelWhenAnyErrorThenQuiesce(
            {ExecutorFuture(getExecutor())
                 .then([&]() {
                     checkSignalReady();
                     executorShutDownTriggered.future.wait();
                     uasserted(6791600, "Executor shut down");
                 })
                 .share(),
             ExecutorFuture(getExecutor())
                 .then([&]() {
                     checkSignalReady();
                     token.onCancel().wait();
                     taskWasCancelled.store(true);
                 })
                 .share()},
            getExecutor(),
            cancelSource);
    });
    taskThreadsReady.future.wait();
    getExecutor()->shutdown();
    executorShutDownTriggered.promise.emplaceValue();
    auto status = quiesced.getNoThrow();
    ASSERT_EQ(status.code(), 6791600);
    ASSERT_TRUE(taskWasCancelled.load());
}
}  // namespace
}  // namespace mongo
