/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/s/chunk_split_state_driver.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

class ChunkSplitStateDriverTestNoTeardown : public unittest::Test {
public:
    // Add bytes to write tracker and create the ChunkSplitStateDriver object
    // to test, which starts the split on the writes tracker
    void setUp() override {
        _writesTracker = std::make_shared<ChunkWritesTracker>();
        uint64_t bytesToAdd{4};
        _writesTracker->addBytesWritten(bytesToAdd);
        _splitDriver = ChunkSplitStateDriver::tryInitiateSplit(_writesTracker);
    }

    void tearDown() override {}

    ChunkWritesTracker& writesTracker() {
        return *_writesTracker;
    }

    std::shared_ptr<ChunkSplitStateDriver>& splitDriver() {
        return _splitDriver;
    }

protected:
    std::shared_ptr<ChunkWritesTracker> _writesTracker;
    std::shared_ptr<ChunkSplitStateDriver> _splitDriver;
};

class ChunkSplitStateDriverTest : public ChunkSplitStateDriverTestNoTeardown {
public:
    void tearDown() override {
        _splitDriver.reset();
        _writesTracker.reset();
    }
};

TEST(ChunkSplitStateDriverTest, InitiateSplitLeavesBytesWrittenUnchanged) {
    auto writesTracker = std::make_shared<ChunkWritesTracker>();
    uint64_t bytesInTrackerBeforeSplit{4};
    writesTracker->addBytesWritten(bytesInTrackerBeforeSplit);

    auto splitDriver = ChunkSplitStateDriver::tryInitiateSplit(writesTracker);

    ASSERT_EQ(writesTracker->getBytesWritten(), bytesInTrackerBeforeSplit);
}

TEST_F(ChunkSplitStateDriverTest, PrepareSplitClearsBytesWritten) {
    splitDriver()->prepareSplit();
    ASSERT_EQ(writesTracker().getBytesWritten(), 0ull);
}

TEST_F(ChunkSplitStateDriverTestNoTeardown,
       PrepareSplitFollowedByDestructorWithoutCommitRestoresBytesWritten) {
    auto bytesInTracker = writesTracker().getBytesWritten();
    splitDriver()->prepareSplit();
    splitDriver().reset();
    ASSERT_EQ(writesTracker().getBytesWritten(), bytesInTracker);
}

TEST_F(ChunkSplitStateDriverTestNoTeardown,
       PrepareSplitFollowedByDestructorWithoutCommitRestoresOldBytesWrittenPlusNewBytesWritten) {
    auto bytesInTracker = writesTracker().getBytesWritten();
    splitDriver()->prepareSplit();
    uint64_t extraBytesToAdd{4};
    writesTracker().addBytesWritten(extraBytesToAdd);
    splitDriver().reset();

    ASSERT_EQ(writesTracker().getBytesWritten(), bytesInTracker + extraBytesToAdd);
}

TEST_F(ChunkSplitStateDriverTestNoTeardown,
       PrepareSplitThenAbandonPrepareFollowedByDestructorWithoutCommitKeepsOnlyNewBytesWritten) {
    auto bytesInTracker = writesTracker().getBytesWritten();
    ASSERT_GT(bytesInTracker, 0ull);

    splitDriver()->prepareSplit();

    uint64_t extraBytesToAdd{4};
    writesTracker().addBytesWritten(extraBytesToAdd);

    // Should clear previous bytes-written estimate that was stashed by prepare, but not new
    // bytes written
    splitDriver()->abandonPrepare();

    splitDriver().reset();

    ASSERT_EQ(writesTracker().getBytesWritten(), extraBytesToAdd);
}

TEST_F(ChunkSplitStateDriverTest,
       PrepareSplitThenAddBytesThenCommitSplitLeavesNewBytesWrittenUnchanged) {
    splitDriver()->prepareSplit();
    uint64_t extraBytesToAdd{4};
    writesTracker().addBytesWritten(extraBytesToAdd);
    splitDriver()->commitSplit();

    ASSERT_EQ(writesTracker().getBytesWritten(), extraBytesToAdd);
}

TEST_F(ChunkSplitStateDriverTest, ShouldSplitReturnsFalseWhenSplitHasBeenPrepared) {
    splitDriver()->prepareSplit();

    uint64_t maxChunkSize{0};
    ASSERT_FALSE(writesTracker().shouldSplit(maxChunkSize));
}

TEST_F(ChunkSplitStateDriverTest, ShouldSplitReturnsFalseEvenAfterCommit) {
    splitDriver()->prepareSplit();
    splitDriver()->commitSplit();

    uint64_t maxChunkSize{0};
    ASSERT_FALSE(writesTracker().shouldSplit(maxChunkSize));
}

TEST_F(ChunkSplitStateDriverTestNoTeardown,
       ShouldSplitReturnsTrueAfterPrepareSplitThenDestruction) {
    splitDriver()->prepareSplit();
    splitDriver().reset();

    uint64_t maxChunkSize{0};
    ASSERT_TRUE(writesTracker().shouldSplit(maxChunkSize));
}

DEATH_TEST_F(ChunkSplitStateDriverTest,
             CommitSplitWhenStartedAndNotPreparedErrors,
             "Invariant failure") {
    splitDriver()->commitSplit();
}

TEST(ChunkSplitStateDriverTest, PrepareErrorsWhenChunkWritesTrackerNoLongerExists) {
    std::shared_ptr<ChunkSplitStateDriver> splitDriver;
    {
        auto writesTracker = std::make_shared<ChunkWritesTracker>();
        uint64_t bytesToAdd{4};
        writesTracker->addBytesWritten(bytesToAdd);
        splitDriver = ChunkSplitStateDriver::tryInitiateSplit(writesTracker);
    }
    ASSERT_THROWS(splitDriver->prepareSplit(), AssertionException);
}

}  // namespace mongo
