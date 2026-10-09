// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_visibility_manager.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

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
    _rs = engine.getRecordStore(/*opCtx=*/nullptr,
                                NamespaceString::kEmpty,
                                /*ident=*/std::string_view(),
                                options,
                                /*uuid=*/boost::none);

    _notifier = _rs->capped()->getInsertNotifier();
    _manager.reInit(_rs.get(), /*initialTs=*/Timestamp(1));
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
    auto iter1 = trackTimestamps(/*timestampRange=*/{3, 4}, /*managerIsEmpty=*/true);
    auto iter2 = trackTimestamps(/*timestampRange=*/{6, 6}, /*managerIsEmpty=*/false);
    auto iter3 = trackTimestamps(/*timestampRange=*/{8, 9}, /*managerIsEmpty=*/false);

    untrackTimestamps(iter1, /*expectedVisibility=*/5);
    untrackTimestamps(iter2, /*expectedVisibility=*/7);
    untrackTimestamps(iter3, /*expectedVisibility=*/9);

    iter1 = trackTimestamps(/*timestampRange=*/{10, 10}, /*managerIsEmpty=*/true);
    iter2 = trackTimestamps(/*timestampRange=*/{12, 13}, /*managerIsEmpty=*/false);
    iter3 = trackTimestamps(/*timestampRange=*/{15, 15}, /*managerIsEmpty=*/false);

    untrackTimestamps(iter1, /*expectedVisibility=*/11);
    untrackTimestamps(iter2, /*expectedVisibility=*/14);
    untrackTimestamps(iter3, /*expectedVisibility=*/15);
}

TEST_F(OplogVisibilityManagerTest, TrackAndUntrackTimestampsLIFO) {
    auto iter1 = trackTimestamps(/*timestampRange=*/{3, 4}, /*managerIsEmpty=*/true);
    auto iter2 = trackTimestamps(/*timestampRange=*/{6, 6}, /*managerIsEmpty=*/false);
    auto iter3 = trackTimestamps(/*timestampRange=*/{8, 9}, /*managerIsEmpty=*/false);

    untrackTimestamps(iter3, /*expectedVisibility=*/2);
    untrackTimestamps(iter2, /*expectedVisibility=*/2);
    untrackTimestamps(iter1, /*expectedVisibility=*/9);

    iter1 = trackTimestamps(/*timestampRange=*/{10, 10}, /*managerIsEmpty=*/true);
    iter2 = trackTimestamps(/*timestampRange=*/{12, 13}, /*managerIsEmpty=*/false);
    iter3 = trackTimestamps(/*timestampRange=*/{15, 15}, /*managerIsEmpty=*/false);

    untrackTimestamps(iter3, /*expectedVisibility=*/9);
    untrackTimestamps(iter2, /*expectedVisibility=*/9);
    untrackTimestamps(iter1, /*expectedVisibility=*/15);
}

using OplogVisibilityManagerTestDeathTest = OplogVisibilityManagerTest;
DEATH_TEST_F(OplogVisibilityManagerTestDeathTest,
             TrackTimestampFirstTimestampLessThanLastTimestamp,
             "invariant") {
    trackTimestamps(/*timestampRange=*/{4, 3}, /*managerIsEmpty=*/true);
}

DEATH_TEST_F(OplogVisibilityManagerTestDeathTest,
             TrackTimestampsFirstLessThanLatestTimeSeen,
             "invariant") {
    trackTimestamps(/*timestampRange=*/{5, 6}, /*managerIsEmpty=*/true);
    trackTimestamps(/*timestampRange=*/{3, 4}, /*managerIsEmpty=*/false);
}

DEATH_TEST_F(OplogVisibilityManagerTestDeathTest,
             AdvanceVisibilityTimestampWhenTrackingOtherTimestamps,
             "invariant") {
    trackTimestamps(/*timestampRange=*/{3, 5}, /*managerIsEmpty=*/true);

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 2);

    _manager.setOplogVisibilityTimestamp(Timestamp(6));
}

TEST_F(OplogVisibilityManagerTest, SetVisibilityTimestampBackward) {
    trackTimestamps(/*timestampRange=*/{4, 5}, /*managerIsEmpty=*/true);

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 3);

    _manager.setOplogVisibilityTimestamp(Timestamp(2));

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 2);
}

TEST_F(OplogVisibilityManagerTest, SetVisibilityTimestampForward) {
    auto it1 = trackTimestamps(/*timestampRange=*/{4, 5}, /*managerIsEmpty=*/true);

    untrackTimestamps(it1, /*expectedVisibility=*/5);

    _manager.setOplogVisibilityTimestamp(Timestamp(10));

    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 10);
}

TEST_F(OplogVisibilityManagerTest, WaitForTimestampToBecomeVisibile) {
    auto opCtxHolder = makeOperationContext();

    auto it1 = trackTimestamps(/*timestampRange=*/{3, 3}, /*managerIsEmpty=*/true);
    auto it2 = trackTimestamps(/*timestampRange=*/{4, 4}, /*managerIsEmpty=*/false);

    bool doneWait = false;

    stdx::thread pushThread([&] {
        _manager.waitForTimestampToBeVisible(opCtxHolder.get(), Timestamp(4));
        doneWait = true;
    });

    sleepFor(Seconds(2));
    ASSERT(!doneWait);

    auto it3 = trackTimestamps(/*timestampRange=*/{5, 5}, /*managerIsEmpty=*/false);
    sleepFor(Seconds(2));
    ASSERT(!doneWait);

    untrackTimestamps(it1, /*expectedVisibility=*/3);
    sleepFor(Seconds(2));
    ASSERT(!doneWait);
    untrackTimestamps(it2, /*expectedVisibility=*/4);

    pushThread.join();
    ASSERT(doneWait);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 4);

    untrackTimestamps(it3, /*expectedVisibility=*/5);
    ASSERT_EQ(_manager.getOplogVisibilityTimestamp().asULL(), 5);
}


}  // namespace
}  // namespace repl
}  // namespace mongo
