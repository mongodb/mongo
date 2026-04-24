/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_observer.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <mutex>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {
namespace {

class MockObserver : public OpTimeObserver {
public:
    void onOpTime(const Timestamp& ts) override {
        std::lock_guard lk(_mutex);
        _timestamps.push_back(ts);
        _cv.notify_all();
    }

    bool waitForCount(size_t count, Milliseconds timeout = Milliseconds(5000)) {
        std::unique_lock lk(_mutex);
        return _cv.wait_for(
            lk, timeout.toSystemDuration(), [&] { return _timestamps.size() >= count; });
    }

    std::vector<Timestamp> timestamps() {
        std::lock_guard lk(_mutex);
        return _timestamps;
    }

    size_t count() {
        std::lock_guard lk(_mutex);
        return _timestamps.size();
    }

private:
    std::mutex _mutex;
    stdx::condition_variable _cv;
    std::vector<Timestamp> _timestamps;
};

class AppliedOpTimeObserverTest : public ReplCoordTest {
protected:
    void setUp() override {
        ReplCoordTest::setUp();
        assertStartSuccess(BSON("_id" << "mySet"
                                      << "version" << 2 << "members"
                                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                               << "node1:12345"))),
                           HostAndPort("node1", 12345));
    }
};

TEST_F(AppliedOpTimeObserverTest, ObserverReceivesNotificationWhenAppliedOpTimeAdvances) {
    auto raw = std::make_unique<MockObserver>();
    auto* ptr = raw.get();
    getReplCoord()->addAppliedOpTimeObserver(std::move(raw));

    replCoordSetMyLastWrittenOpTime(OpTime(Timestamp(1, 1), 1));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(1, 1), 1));

    ASSERT_TRUE(ptr->waitForCount(1));
    ASSERT_EQ(Timestamp(1, 1), ptr->timestamps()[0]);
}

TEST_F(AppliedOpTimeObserverTest, ObserverNotNotifiedForNullOpTime) {
    auto raw = std::make_unique<MockObserver>();
    auto* ptr = raw.get();
    getReplCoord()->addAppliedOpTimeObserver(std::move(raw));

    // Setting null optime should not trigger the observer.
    getReplCoord()->resetMyLastOpTimes();

    // Advance to a real timestamp to confirm the observer is functional.
    replCoordSetMyLastWrittenOpTime(OpTime(Timestamp(2, 1), 1));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(2, 1), 1));

    ASSERT_TRUE(ptr->waitForCount(1));
    ASSERT_EQ(1U, ptr->count());
    ASSERT_EQ(Timestamp(2, 1), ptr->timestamps()[0]);
}

TEST_F(AppliedOpTimeObserverTest, MultipleObserversAllReceiveNotification) {
    auto raw1 = std::make_unique<MockObserver>();
    auto raw2 = std::make_unique<MockObserver>();
    auto* ptr1 = raw1.get();
    auto* ptr2 = raw2.get();
    getReplCoord()->addAppliedOpTimeObserver(std::move(raw1));
    getReplCoord()->addAppliedOpTimeObserver(std::move(raw2));

    replCoordSetMyLastWrittenOpTime(OpTime(Timestamp(5, 1), 1));
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(5, 1), 1));

    ASSERT_TRUE(ptr1->waitForCount(1));
    ASSERT_TRUE(ptr2->waitForCount(1));
    ASSERT_EQ(Timestamp(5, 1), ptr1->timestamps()[0]);
    ASSERT_EQ(Timestamp(5, 1), ptr2->timestamps()[0]);
}

TEST_F(AppliedOpTimeObserverTest, ObserverReceivesMultipleAdvancementsInOrder) {
    auto raw = std::make_unique<MockObserver>();
    auto* ptr = raw.get();
    getReplCoord()->addAppliedOpTimeObserver(std::move(raw));

    std::vector<Timestamp> expected = {
        Timestamp(1, 1), Timestamp(2, 1), Timestamp(3, 1), Timestamp(4, 1), Timestamp(5, 1)};

    for (size_t i = 0; i < expected.size(); ++i) {
        replCoordSetMyLastWrittenOpTime(OpTime(expected[i], 1));
        replCoordSetMyLastAppliedOpTime(OpTime(expected[i], 1));
        // Wait for each notification before advancing further to prevent coalescing.
        ASSERT_TRUE(ptr->waitForCount(i + 1));
    }

    auto got = ptr->timestamps();
    ASSERT_EQ(expected.size(), got.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], got[i]);
    }
}

}  // namespace
}  // namespace repl
}  // namespace mongo
