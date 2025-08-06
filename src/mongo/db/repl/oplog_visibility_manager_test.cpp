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

#include "mongo/db/repl/oplog_visibility_manager.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class OplogVisibilityManagerTest : public ServiceContextMongoDTest {
public:
    OplogVisibilityManagerTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}
    void setUp() override;
    void tearDown() override;
    OplogVisibilityManager::const_iterator trackTimestamps(
        const std::pair<std::size_t, std::size_t>& timestampRange, bool managerIsEmpty);
    /**
     * Untracks the Timestamp pos points to. Also asserts to make sure the new oplog visibility
     * timestamp is the expected visibility timestamp.
     */
    void untrackTimestamps(OplogVisibilityManager::const_iterator pos,
                           std::size_t expectedVisibility);

protected:
    OplogVisibilityManager _manager;
    std::unique_ptr<RecordStore> _rs;
    std::shared_ptr<CappedInsertNotifier> _notifier;
};

void OplogVisibilityManagerTest::setUp() {
    ServiceContextMongoDTest::setUp();
    RecordStore::Options options;
    options.isCapped = true;
    DevNullKVEngine engine{};
    _rs = engine.getRecordStore(nullptr /* opCtx */,
                                NamespaceString::kEmpty,
                                StringData() /* ident */,
                                options,
                                boost::none /* uuid */);

    _notifier = _rs->capped()->getInsertNotifier();
    _manager.reInit(_rs.get(), Timestamp(1) /* initialTs */);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp(), Timestamp(1));
}

void OplogVisibilityManagerTest::tearDown() {
    ServiceContextMongoDTest::tearDown();
}

// Wrapper function that validates that tracking a timestamp range does not move or
// alter the oplog visibility timestamp unless the manager is empty.
OplogVisibilityManager::const_iterator OplogVisibilityManagerTest::trackTimestamps(
    const std::pair<std::size_t, std::size_t>& timestampRange, bool managerIsEmpty) {
    const auto first = Timestamp(timestampRange.first);
    const auto last = Timestamp(timestampRange.second);
    auto expectedVisibility = managerIsEmpty ? (first - 1) : _manager.getOplogVisibilityTimestamp();
    auto res = _manager.trackTimestamps(first, last);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp(), expectedVisibility);
    return res;
}

void OplogVisibilityManagerTest::untrackTimestamps(OplogVisibilityManager::const_iterator pos,
                                                   std::size_t expectedVisibility) {
    const auto prevVisibilityTs = _manager.getOplogVisibilityTimestamp();
    const auto prevVersion = _manager.getRecordStore()->capped()->getInsertNotifier()->getVersion();
    _manager.untrackTimestamps(pos);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp(), Timestamp(expectedVisibility));
    if (_manager.getOplogVisibilityTimestamp() > prevVisibilityTs) {
        ASSERT_GT(_manager.getRecordStore()->capped()->getInsertNotifier()->getVersion(),
                  prevVersion);
    }
}

TEST_F(OplogVisibilityManagerTest, TrackAndUntrackTimestampsFIFO) {
    auto iter1 = trackTimestamps({3, 4} /* timestampRange */, true /* managerIsEmpty */);
    auto iter2 = trackTimestamps({6, 6} /* timestampRange */, false /* managerIsEmpty */);
    auto iter3 = trackTimestamps({8, 9} /* timestampRange */, false /* managerIsEmpty */);

    untrackTimestamps(iter1, 5 /* expectedVisibility */);
    untrackTimestamps(iter2, 7 /* expectedVisibility */);
    untrackTimestamps(iter3, 9 /* expectedVisibility */);

    iter1 = trackTimestamps({10, 10} /* timestampRange */, true /* managerIsEmpty */);
    iter2 = trackTimestamps({12, 13} /* timestampRange */, false /* managerIsEmpty */);
    iter3 = trackTimestamps({15, 15} /* timestampRange */, false /* managerIsEmpty */);

    untrackTimestamps(iter1, 11 /* expectedVisibility */);
    untrackTimestamps(iter2, 14 /* expectedVisibility */);
    untrackTimestamps(iter3, 15 /* expectedVisibility */);
}

TEST_F(OplogVisibilityManagerTest, TrackAndUntrackTimestampsLIFO) {
    auto iter1 = trackTimestamps({3, 4} /* timestampRange */, true /* managerIsEmpty */);
    auto iter2 = trackTimestamps({6, 6} /* timestampRange */, false /* managerIsEmpty */);
    auto iter3 = trackTimestamps({8, 9} /* timestampRange */, false /* managerIsEmpty */);

    untrackTimestamps(iter3, 2 /* expectedVisibility */);
    untrackTimestamps(iter2, 2 /* expectedVisibility */);
    untrackTimestamps(iter1, 9 /* expectedVisibility */);

    iter1 = trackTimestamps({10, 10} /* timestampRange */, true /* managerIsEmpty */);
    iter2 = trackTimestamps({12, 13} /* timestampRange */, false /* managerIsEmpty */);
    iter3 = trackTimestamps({15, 15} /* timestampRange */, false /* managerIsEmpty */);

    untrackTimestamps(iter3, 9 /* expectedVisibility */);
    untrackTimestamps(iter2, 9 /* expectedVisibility */);
    untrackTimestamps(iter1, 15 /* expectedVisibility */);
}

DEATH_TEST_F(OplogVisibilityManagerTest,
             TrackTimestampFirstTimestampLessThanLastTimestamp,
             "invariant") {
    trackTimestamps({4, 3} /* timestampRange */, true /* managerIsEmpty */);
}

DEATH_TEST_F(OplogVisibilityManagerTest, TrackTimestampsFirstLessThanLatestTimeSeen, "invariant") {
    trackTimestamps({5, 6} /* timestampRange */, true /* managerIsEmpty */);
    trackTimestamps({3, 4} /* timestampRange */, false /* managerIsEmpty */);
}

DEATH_TEST_F(OplogVisibilityManagerTest,
             AdvanceVisibilityTimestampWhenTrackingOtherTimestamps,
             "invariant") {
    trackTimestamps({3, 5} /* timestampRange */, true /* managerIsEmpty */);

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 2);

    _manager.setOplogVisibilityTimestamp(Timestamp(6));
}

TEST_F(OplogVisibilityManagerTest, SetVisibilityTimestampBackward) {
    trackTimestamps({4, 5} /* timestampRange */, true /* managerIsEmpty */);

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 3);

    _manager.setOplogVisibilityTimestamp(Timestamp(2));

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 2);
}

TEST_F(OplogVisibilityManagerTest, SetVisibilityTimestampForward) {
    auto it1 = trackTimestamps({4, 5} /* timestampRange */, true /* managerIsEmpty */);

    untrackTimestamps(it1, 5 /* expectedVisibility */);

    _manager.setOplogVisibilityTimestamp(Timestamp(10));

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 10);
}

TEST_F(OplogVisibilityManagerTest, WaitForTimestampToBecomeVisibile) {
    auto opCtxHolder = makeOperationContext();

    auto it1 = trackTimestamps({3, 3} /* timestampRange */, true /* managerIsEmpty */);
    auto it2 = trackTimestamps({4, 4} /* timestampRange */, false /* managerIsEmpty */);

    bool doneWait = false;

    stdx::thread pushThread([&] {
        _manager.waitForTimestampToBeVisible(opCtxHolder.get(), Timestamp(4));
        doneWait = true;
    });

    sleepFor(Seconds(2));
    ASSERT(!doneWait);

    auto it3 = trackTimestamps({5, 5} /* timestampRange */, false /* managerIsEmpty */);
    sleepFor(Seconds(2));
    ASSERT(!doneWait);

    untrackTimestamps(it1, 3 /* expectedVisibility */);
    sleepFor(Seconds(2));
    ASSERT(!doneWait);
    untrackTimestamps(it2, 4 /* expectedVisibility */);

    pushThread.join();
    ASSERT(doneWait);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 4);

    untrackTimestamps(it3, 5 /* expectedVisibility */);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 5);
}


}  // namespace
}  // namespace repl
}  // namespace mongo
