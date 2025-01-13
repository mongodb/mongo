/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {

class ReactorTestFixture : public unittest::Test {
public:
    virtual TransportLayer* getTransportLayer() = 0;

    void runTestWithReactor(
        std::function<void(ReactorHandle, Atomic<bool>&, Notification<void>*)> testBody) {
        Atomic<bool> inDraining{false};
        Notification<void> shutdown;
        auto reactor = getTransportLayer()->getReactor(TransportLayer::kNewReactor);
        auto reactorThread = stdx::thread([&] {
            setThreadName(fmt::format("{}-reactor-test", getTransportLayer()->getNameForLogging()));
            LOGV2(9715100, "running reactor");
            reactor->run();
            LOGV2(9715101, "reactor stopped, draining");
            inDraining.swap(true);
            reactor->drain();
            LOGV2(9715102, "reactor drain complete");
            shutdown.set();
        });

        testBody(reactor, inDraining, &shutdown);

        reactorThread.join();
    }

    void testBasicSchedule() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto pf = makePromiseFuture<void>();
            reactor->schedule([&](Status status) { pf.promise.setFrom(status); });

            ASSERT_OK(std::move(pf.future).getNoThrow());

            reactor->stop();
            shutdown->get();
        });
    }

    void testDrainTask() {
        runTestWithReactor(
            [](ReactorHandle reactor, Atomic<bool>& inDraining, Notification<void>* shutdown) {
                Notification<void> stopCalled;
                auto pf1 = makePromiseFuture<bool>();
                reactor->schedule([&](Status status) {
                    stopCalled.get();

                    if (!status.isOK()) {
                        pf1.promise.setError(status);
                    }
                    pf1.promise.setWith([&] { return inDraining.load(); });
                });

                auto pf2 = makePromiseFuture<bool>();
                reactor->schedule([&](Status status) {
                    stopCalled.get();

                    if (!status.isOK()) {
                        pf2.promise.setError(status);
                    }
                    pf2.promise.setWith([&] { return inDraining.load(); });
                });

                reactor->stop();
                stopCalled.set();

                /**
                 * The only guaranteed ordering between the first task running, the second task
                 * running, and stop() getting called is that at least one task will run after
                 * stop() was called (and stopCalled was set). That is ok, because this test just
                 * cares that at least one task was executed during drain()-- because of this, we
                 * just assert that both tasks ran successfully and that at least one of the tasks
                 * executed while inDraining is true.
                 */
                StatusWith<bool> res1 = std::move(pf1.future).getNoThrow();
                StatusWith<bool> res2 = std::move(pf2.future).getNoThrow();
                ASSERT_OK(res1.getStatus());
                ASSERT_OK(res2.getStatus());
                ASSERT_TRUE(res1.getValue() || res2.getValue());

                shutdown->get();
            });
    }

    void testRecordsTaskStats() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto pf = makePromiseFuture<void>();
            reactor->schedule([&](Status status) { pf.promise.setFrom(status); });

            ASSERT_OK(std::move(pf.future).getNoThrow());

            // Give the reactor stats enough time to update.
            sleepmillis(100);

            // No need to test the functionality of ExecutorStats, but we should ensure the reactor
            // properly monitors tasks via wrapTask.
            BSONObjBuilder bob;
            reactor->appendStats(bob);
            auto stats = bob.obj();
            ASSERT_EQ(stats.getField("scheduled").numberLong(), 1);
            ASSERT_EQ(stats.getField("executed").numberLong(), 1);

            reactor->stop();
            shutdown->get();
        });
    }

    void testOnReactorThread() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto pf = makePromiseFuture<bool>();
            reactor->schedule([&](Status status) {
                pf.promise.setWith([&] { return reactor->onReactorThread(); });
            });

            ASSERT_TRUE(std::move(pf.future).getNoThrow().getValue());
            ASSERT_FALSE(reactor->onReactorThread());

            reactor->stop();
            shutdown->get();
        });
    }

    void testScheduleOnReactorAfterShutdownFails() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            Notification<void> firstTask;
            reactor->schedule([&](Status s) {
                LOGV2(9715103, "first task scheduled", "status"_attr = s);
                firstTask.set();
            });
            firstTask.get();

            LOGV2(9715104, "shutting down reactor");
            reactor->stop();
            shutdown->get();

            auto pf = makePromiseFuture<void>();
            reactor->schedule([&](Status status) { pf.promise.setFrom(status); });

            ASSERT_EQ(std::move(pf.future).getNoThrow(), ErrorCodes::ShutdownInProgress);
        });
    }

    void testBasicTimer() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto timer = reactor->makeTimer();

            auto pf = makePromiseFuture<void>();
            timer->waitUntil(Date_t::now() + Seconds(1)).getAsync([&](Status s) {
                std::move(pf.promise).setFrom(s);
            });

            ASSERT_OK(std::move(pf.future).getNoThrow());

            reactor->stop();
            shutdown->get();
        });
    }

    void testBasicTimerCancel() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto timer = reactor->makeTimer();

            auto pf = makePromiseFuture<void>();
            timer->waitUntil(Date_t::now() + Seconds(1)).getAsync([&](Status s) {
                std::move(pf.promise).setFrom(s);
            });

            timer->cancel();

            ASSERT_EQ(std::move(pf.future).getNoThrow().code(), ErrorCodes::CallbackCanceled);

            reactor->stop();
            shutdown->get();
        });
    }

    void testSchedulingTwiceOnTimerCancelsFirstOne() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto timer = reactor->makeTimer();

            auto pf1 = makePromiseFuture<void>();
            timer->waitUntil(Date_t::now() + Seconds(1)).getAsync([&](Status s) {
                std::move(pf1.promise).setFrom(s);
            });

            auto pf2 = makePromiseFuture<void>();
            timer->waitUntil(Date_t::now() + Seconds(1)).getAsync([&](Status s) {
                std::move(pf2.promise).setFrom(s);
            });

            ASSERT_EQ(std::move(pf1.future).getNoThrow().code(), ErrorCodes::CallbackCanceled);
            ASSERT_OK(std::move(pf2.future).getNoThrow());

            reactor->stop();
            shutdown->get();
        });
    }

    void testUseTimerAfterReactorShutdown() {
        runTestWithReactor([](ReactorHandle reactor, Atomic<bool>&, Notification<void>* shutdown) {
            auto timer = reactor->makeTimer();
            auto timer1 = timer->waitUntil(Date_t::now() + Seconds(1));
            ASSERT_OK(std::move(timer1).getNoThrow());

            reactor->stop();
            shutdown->get();

            auto timer2 = timer->waitUntil(Date_t::now() + Seconds(1));
            ASSERT_EQ(std::move(timer2).getNoThrow().code(), ErrorCodes::ShutdownInProgress);
        });
    }

    void testSafeToUseTimerAfterReactorDestruction() {
        // This test doesn't use the runTestWithReactor helper because it needs sole ownership over
        // the reactor to delete it.
        Notification<void> shutdown;
        auto reactor = getTransportLayer()->getReactor(TransportLayer::kNewReactor);
        auto reactorThread = stdx::thread([&] {
            setThreadName(fmt::format("{}-reactor", getTransportLayer()->getNameForLogging()));
            reactor->run();
            reactor->drain();
            shutdown.set();
        });

        auto timer = reactor->makeTimer();

        // Stop, drain, and delete the reactor.
        reactor->stop();
        shutdown.get();
        reactor.reset();

        ASSERT_EQ(reactor, nullptr);

        auto timer1 = timer->waitUntil(Date_t::now() + Seconds(1));
        ASSERT_EQ(std::move(timer1).getNoThrow().code(), ErrorCodes::ShutdownInProgress);

        reactorThread.join();
    }
};

}  // namespace mongo::transport
