// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/clean_shutdown_checker.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/clean_shutdown_gen.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <mutex>

namespace mongo {
namespace repl {
namespace {

const HostAndPort kSyncSource("sync-source", 27017);
const Timestamp kBeginApplyingTs(100, 1);

BSONObj makeCleanShutdownDoc(long long id, Timestamp lastCheckpointTs) {
    CleanShutdownDocument doc;
    doc.setId(id);
    doc.setCleanShutdownLastCheckpointTimestamp(lastCheckpointTs);

    BSONObjBuilder bob;
    doc.serialize(&bob);
    return bob.obj();
}

TEST(CleanShutdownCheckerTest, BaselineFindLooksForTheMostRecentShutdown) {
    auto cmd = makeCleanShutdownBaselineFindCmd();
    ASSERT_EQUALS(-1, cmd["sort"]["_id"].numberInt());
    ASSERT_EQUALS(1, cmd["limit"].numberInt());
    ASSERT_FALSE(cmd.hasField("filter"));
}

TEST(CleanShutdownCheckerTest, CheckFindLooksForTheFirstShutdownAfterTheBaseline) {
    auto cmd = makeCleanShutdownCheckFindCmd(4);
    ASSERT_BSONOBJ_EQ(BSON("$gte" << 5LL), cmd["filter"]["_id"].Obj());
    ASSERT_EQUALS(1, cmd["sort"]["_id"].numberInt());
    ASSERT_EQUALS(1, cmd["limit"].numberInt());
}

TEST(CleanShutdownCheckerTest, CheckFindFromTheNoShutdownBaselineStartsAtZero) {
    // A sync source that has never cleanly restarted writes _id 0 the first time it does, so the
    // baseline must sit one below it for that first shutdown to be caught.
    auto cmd = makeCleanShutdownCheckFindCmd(kNoCleanShutdownId);
    ASSERT_BSONOBJ_EQ(BSON("$gte" << 0LL), cmd["filter"]["_id"].Obj());
}

TEST(CleanShutdownCheckerTest, BaselineIsTheIdOfTheReturnedDocument) {
    ASSERT_EQUALS(12LL,
                  unittest::assertGet(
                      parseCleanShutdownBaseline({makeCleanShutdownDoc(12, Timestamp(5, 1))})));
}

TEST(CleanShutdownCheckerTest, EmptyBaselineResponseMeansNoRecordedShutdown) {
    // A sync source that seeds the sentinel never answers with nothing, so this is a sync source
    // too old to have the collection at all. It cannot record a shutdown for a check to match
    // either, so it takes the same baseline.
    ASSERT_EQUALS(kNoCleanShutdownId, unittest::assertGet(parseCleanShutdownBaseline({})));
}

TEST(CleanShutdownCheckerTest, SentinelBaselineMeansNoRecordedShutdown) {
    // What a node that has never cleanly shut down actually reports. It has to read the same as an
    // absent document, and leave the first real shutdown at _id 0 still to be caught.
    auto sentinel = makeCleanShutdownDoc(kNoCleanShutdownId, Timestamp());
    ASSERT_EQUALS(kNoCleanShutdownId, unittest::assertGet(parseCleanShutdownBaseline({sentinel})));
}

TEST(CleanShutdownCheckerTest, MalformedBaselineDocumentIsAnError) {
    ASSERT_NOT_OK(
        parseCleanShutdownBaseline({BSON("_id" << 3LL << "unexpected" << 1)}).getStatus());
}

TEST(CleanShutdownCheckerTest, NoDocumentMeansNothingHappenedSinceTheBaseline) {
    ASSERT_OK(checkCleanShutdownResult(boost::none, 7, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, ShutdownWithACheckpointPastBeginApplyingIsSafe) {
    // Everything before the checkpoint survived the restart and everything at or after
    // beginApplyingTimestamp is covered by oplog replay, so the vulnerable window is empty.
    auto doc = makeCleanShutdownDoc(8, Timestamp(200, 1));
    ASSERT_OK(checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, ShutdownWithACheckpointExactlyAtBeginApplyingIsSafe) {
    // The boundary closes the window rather than leaving it open by one timestamp.
    auto doc = makeCleanShutdownDoc(8, kBeginApplyingTs);
    ASSERT_OK(checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, ShutdownWithACheckpointBeforeBeginApplyingFailsTheAttempt) {
    auto doc = makeCleanShutdownDoc(8, Timestamp(50, 1));
    auto status = checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, status);
    ASSERT_STRING_CONTAINS(status.reason(), "cleanly shut down during initial sync");
}

TEST(CleanShutdownCheckerTest, NullCheckpointTimestampFailsTheAttempt) {
    // A node that never took a stable checkpoint records a null timestamp, which must be read as
    // "cannot prove this was safe" rather than as an absent value.
    auto doc = makeCleanShutdownDoc(8, Timestamp());
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure,
                  checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, FirstEverShutdownIsCaughtFromTheNoShutdownBaseline) {
    auto doc = makeCleanShutdownDoc(0, Timestamp(50, 1));
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure,
                  checkCleanShutdownResult(doc, kNoCleanShutdownId, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, TruncatedHistoryFailsTheAttempt) {
    // The shutdown at _id 8 aged out of the capped collection, so whether it was safe is
    // unknowable. That is a failure, not a pass.
    auto doc = makeCleanShutdownDoc(9, Timestamp(200, 1));
    auto status = checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, status);
    ASSERT_STRING_CONTAINS(status.reason(), "truncated away");
}

TEST(CleanShutdownCheckerTest, TruncatedHistoryFailsEvenWithASafeCheckpoint) {
    // The returned document's own timestamp says nothing about the one that was truncated, so a
    // late checkpoint must not rescue this case.
    auto doc = makeCleanShutdownDoc(20, Timestamp(10000, 1));
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure,
                  checkCleanShutdownResult(doc, 7, kBeginApplyingTs, kSyncSource));
}

TEST(CleanShutdownCheckerTest, MalformedCheckDocumentIsAnError) {
    ASSERT_NOT_OK(checkCleanShutdownResult(BSON("_id" << 8LL), 7, kBeginApplyingTs, kSyncSource));
}

BSONObj makeFindResponse(const std::vector<BSONObj>& docs) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursor(bob.subobjStart("cursor"));
        cursor.append("id", 0LL);
        cursor.append("ns", "local.system.cleanShutdownLog");
        BSONArrayBuilder batch(cursor.subarrayStart("firstBatch"));
        for (auto&& doc : docs) {
            batch.append(doc);
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

/**
 * Drives the checker's remote commands against a mock network, so that the state it carries from
 * one check to the next can be observed.
 */
class CleanShutdownCheckerFixture : public executor::ThreadPoolExecutorTest {
protected:
    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
        _checker = std::make_unique<CleanShutdownChecker>(&getExecutor(), kSyncSource);
    }

    CleanShutdownChecker* checker() {
        return _checker.get();
    }

    /**
     * Answers the checker's outstanding find with 'response', asserting that it issued one.
     */
    void respondTo(const BSONObj& response) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        ASSERT_TRUE(getNet()->hasReadyRequests());
        getNet()->scheduleSuccessfulResponse(response);
        getNet()->runReadyNetworkOperations();
    }

    /**
     * Stands in for the sync source only if the checker asks, and reports whether it did. Answering
     * a check that should never have been issued keeps a checker that asks anyway from hanging the
     * test on a callback that never runs, so it fails on the assertion instead.
     */
    bool respondIfAsked(const BSONObj& response) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(getNet());
        if (!getNet()->hasReadyRequests()) {
            return false;
        }
        getNet()->scheduleSuccessfulResponse(response);
        getNet()->runReadyNetworkOperations();
        return true;
    }

    /**
     * Records the baseline as 'baseId' by answering reset()'s find with a document carrying it.
     */
    void resetBaselineTo(long long baseId) {
        auto status = recordStatus();
        auto cbh = unittest::assertGet(_checker->reset(status.first));
        respondTo(makeFindResponse({makeCleanShutdownDoc(baseId, Timestamp(1, 1))}));
        getExecutor().wait(cbh);
        ASSERT_OK(*status.second);
        ASSERT_EQUALS(baseId, _checker->getBaseCleanShutdownId());
    }

    /**
     * A callback that stores the status it is called with, and the location it stores it in. The
     * status starts out as NotYetInitialized so that a callback that never runs is visible.
     */
    std::pair<CleanShutdownChecker::CallbackFn, std::shared_ptr<Status>> recordStatus() {
        auto result = std::make_shared<Status>(ErrorCodes::NotYetInitialized, "");
        auto callback = [this, result](const Status& status) {
            std::lock_guard<std::mutex> lk(_mutex);
            *result = status;
        };
        return {callback, result};
    }

    std::mutex _mutex;
    std::unique_ptr<CleanShutdownChecker> _checker;
};

TEST_F(CleanShutdownCheckerFixture, CheckAfterAClearedShutdownDoesNotQueryTheSyncSourceAgain) {
    resetBaselineTo(7);

    // The first check sees the shutdown at _id 8 and clears it, because its checkpoint is past
    // beginApplyingTimestamp.
    auto first = recordStatus();
    auto firstCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, first.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(200, 1))}));
    getExecutor().wait(firstCbh);
    ASSERT_OK(*first.second);

    // The second finds the answer already settled, so it never reaches the network.
    auto second = recordStatus();
    auto secondCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, second.first));
    auto asked = respondIfAsked(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(200, 1))}));
    getExecutor().wait(secondCbh);
    ASSERT_FALSE(asked);
    ASSERT_OK(*second.second);
}

TEST_F(CleanShutdownCheckerFixture, ClearedShutdownSurvivesItsDocumentBeingTruncatedAway) {
    // This is what the capped collection overflowing looks like: the shutdown at _id 8 was checked
    // and cleared, then aged out before the attempt's final check. Asking again would report it as
    // truncated history and fail an attempt already known to be safe, so the check must not ask.
    resetBaselineTo(7);

    auto first = recordStatus();
    auto firstCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, first.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(200, 1))}));
    getExecutor().wait(firstCbh);
    ASSERT_OK(*first.second);

    auto second = recordStatus();
    auto secondCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, second.first));

    // Answer the way a sync source whose capped collection has since truncated _id 8 away would,
    // but only if the checker asks. A settled answer means it never asks, and so never sees this.
    respondIfAsked(makeFindResponse({makeCleanShutdownDoc(20, Timestamp(300, 1))}));
    getExecutor().wait(secondCbh);
    ASSERT_OK(*second.second);
}

TEST_F(CleanShutdownCheckerFixture, CheckAfterNoShutdownStillQueriesTheSyncSource) {
    // Nothing was cleared, because nothing happened. A shutdown after this check still has to be
    // caught, so the next check must go back to the sync source.
    resetBaselineTo(7);

    auto first = recordStatus();
    auto firstCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, first.first));
    respondTo(makeFindResponse({}));
    getExecutor().wait(firstCbh);
    ASSERT_OK(*first.second);

    auto second = recordStatus();
    auto secondCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, second.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(50, 1))}));
    getExecutor().wait(secondCbh);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, *second.second);
}

TEST_F(CleanShutdownCheckerFixture, CheckAfterAnUnsafeShutdownStillQueriesTheSyncSource) {
    // A failed check is not a settled answer, so it must not stop later checks from running.
    resetBaselineTo(7);

    auto first = recordStatus();
    auto firstCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, first.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(50, 1))}));
    getExecutor().wait(firstCbh);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, *first.second);

    auto second = recordStatus();
    auto secondCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, second.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(50, 1))}));
    getExecutor().wait(secondCbh);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, *second.second);
}

TEST_F(CleanShutdownCheckerFixture, ResetClearsASettledAnswer) {
    // A new baseline is a new question, and what the old one settled says nothing about it.
    resetBaselineTo(7);

    auto first = recordStatus();
    auto firstCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, first.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(8, Timestamp(200, 1))}));
    getExecutor().wait(firstCbh);
    ASSERT_OK(*first.second);

    resetBaselineTo(20);

    auto second = recordStatus();
    auto secondCbh =
        unittest::assertGet(checker()->checkForCleanShutdown(kBeginApplyingTs, second.first));
    respondTo(makeFindResponse({makeCleanShutdownDoc(21, Timestamp(50, 1))}));
    getExecutor().wait(secondCbh);
    ASSERT_EQUALS(ErrorCodes::InitialSyncFailure, *second.second);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
