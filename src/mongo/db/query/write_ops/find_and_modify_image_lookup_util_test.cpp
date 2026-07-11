// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {
namespace {

const auto kMaxTsInc = std::numeric_limits<unsigned>::max();
const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("testDb", "testColl");

repl::OplogEntry makeOplogEntry(repl::OpTime entryOpTime,
                                repl::OpTypeEnum opType,
                                BSONObj oField,
                                boost::optional<BSONObj> o2Field,
                                OperationSessionInfo sessionInfo,
                                std::vector<StmtId> stmtIds,
                                boost::optional<RetryImageEnum> needsRetryImage) {
    return {
        repl::DurableOplogEntry(entryOpTime,                      // optime
                                opType,                           // opType
                                kNss,                             // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                boost::none,                      // checkExistenceForDiffInsert
                                boost::none,                      // versionContext
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                sessionInfo,
                                boost::none,  // upsert
                                Date_t(),     // wall clock time
                                stmtIds,
                                boost::none,  // optime of previous write within same transaction
                                boost::none,  // pre-image optime
                                boost::none,  // post-image optime
                                boost::none,  // ShardId of resharding recipient
                                boost::none,  // _id
                                needsRetryImage)};  // needsRetryImage
}

void testFetchPreOrPostImageFromSnapshot(
    RetryImageEnum imageType,
    const repl::OpTime& entryOpTime,
    const boost::optional<Timestamp>& commitTimestamp,
    const boost::optional<Timestamp>& expectedAtClusterTime,
    const boost::optional<int>& expectedErrorCode = boost::none) {
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionIdForTest());
    sessionInfo.setTxnNumber(0);

    auto preImageDoc = BSON("_id" << 0);
    auto docKey = BSON("_id" << 0);
    auto postImageDoc = BSON("_id" << 0 << "x" << 0);
    auto entry = makeOplogEntry(entryOpTime,
                                repl::OpTypeEnum::kUpdate,
                                postImageDoc,
                                docKey,
                                sessionInfo,
                                {1} /* stmtIds */,
                                imageType);  // needsRetryImage
    if (commitTimestamp) {
        entry.setCommitTransactionTimestamp(*commitTimestamp);
    }

    auto findOneLocallyFunc =
        [&](const NamespaceString& nss,
            const BSONObj& filter,
            const boost::optional<repl::ReadConcernArgs>& readConcern) -> boost::optional<BSONObj> {
        if (expectedErrorCode) {
            FAIL("Expected the timestamp validation step to hit and fail before the findOne step");
            MONGO_UNREACHABLE;
        }
        ASSERT_EQ(readConcern->getLevel(), ReadConcernLevelEnum::kSnapshotReadConcern);
        auto atClusterTime = readConcern->getArgsAtClusterTime();
        ASSERT(atClusterTime);
        ASSERT_EQ(atClusterTime->asTimestamp(), *expectedAtClusterTime);
        return imageType == RetryImageEnum::kPreImage ? preImageDoc : postImageDoc;
    };

    if (expectedErrorCode) {
        ASSERT_THROWS_CODE(fetchPreOrPostImageFromSnapshot(entry, findOneLocallyFunc),
                           DBException,
                           *expectedErrorCode);
    } else {
        ASSERT(expectedAtClusterTime);
        auto doc = fetchPreOrPostImageFromSnapshot(entry, findOneLocallyFunc);
        ASSERT(doc);
        ASSERT_BSONOBJ_EQ(*doc,
                          imageType == RetryImageEnum::kPreImage ? preImageDoc : postImageDoc);
    }
}

TEST(FindAndModifyImageLookupTest, FetchPreImageFromSnapshotBasic_RetryableWrite) {
    auto entryOpTime0 = repl::OpTime(Timestamp(100, 2), 5);
    auto expectedAtClusterTime0 = Timestamp(100, 1);
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPreImage,
                                        entryOpTime0,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime0);

    auto entryOpTime1 = repl::OpTime(Timestamp(100, 0), 5);
    auto expectedAtClusterTime1 = Timestamp(99, kMaxTsInc);
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPreImage,
                                        entryOpTime1,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime1);
}

TEST(FindAndModifyImageLookupTest, FetchPreImageFromSnapshotBasic_TransactionOplogTsEqCommitTxnTs) {
    auto entryOpTime0 = repl::OpTime(Timestamp(100, 1), 5);
    auto commitTimestamp0 = Timestamp(100, 1);
    auto expectedAtClusterTime0 = Timestamp(100, 0);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime0, commitTimestamp0, expectedAtClusterTime0);

    auto entryOpTime1 = repl::OpTime(Timestamp(100, 0), 5);
    auto commitTimestamp1 = Timestamp(100, 0);
    auto expectedAtClusterTime1 = Timestamp(99, kMaxTsInc);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime1, commitTimestamp1, expectedAtClusterTime1);
}

TEST(FindAndModifyImageLookupTest,
     FetchPreImageFromSnapshotBasic_TransactionOplogTsNeqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(100, 1), 5);
    auto commitTimestamp = Timestamp(100, 2);
    auto expectedAtClusterTime = Timestamp(100, 1);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime, commitTimestamp, expectedAtClusterTime);

    auto entryOpTime1 = repl::OpTime(Timestamp(99, 1), 5);
    auto commitTimestamp1 = Timestamp(100, 0);
    auto expectedAtClusterTime1 = Timestamp(99, kMaxTsInc);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime1, commitTimestamp1, expectedAtClusterTime1);
}

TEST(FindAndModifyImageLookupTest, FetchPreImageFromSnapshotRequiresTsULLGte1_RetryableWrite) {
    auto entryOpTime1 = repl::OpTime(Timestamp(0, 0), 5);
    auto expectedAtClusterTime = boost::none;
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPreImage,
                                        entryOpTime1,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime,
                                        12020800 /* expectedErrorCode */);
}

TEST(FindAndModifyImageLookupTest,
     FetchPreImageFromSnapshotDoesNotRequireTsULLGte2_RetryableWrite) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 1), 5);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPreImage,
                                        entryOpTime,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPreImageFromSnapshotDoesNotRequireTsULLGte2_TransactionOplogTsEqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 1), 5);
    auto commitTimestamp = Timestamp(0, 1);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPreImageFromSnapshotDoesNotRequireTsULLGte2_TransactionOplogTsNeqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 0), 5);
    auto commitTimestamp = Timestamp(0, 1);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPreImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest, FetchPostImageFromSnapshotBasic_RetryableWrite) {
    auto entryOpTime = repl::OpTime(Timestamp(100, 2), 5);
    auto expectedAtClusterTime = Timestamp(100, 2);
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPostImage,
                                        entryOpTime,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPostImageFromSnapshotBasic_TransactionOplogTsEqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(100, 2), 5);
    auto commitTimestamp = Timestamp(100, 2);
    auto expectedAtClusterTime = Timestamp(100, 2);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPostImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPostImageFromSnapshotBasic_TransactionOplogTsNeqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(100, 1), 5);
    auto commitTimestamp = Timestamp(100, 2);
    auto expectedAtClusterTime = Timestamp(100, 2);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPostImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPostImageFromSnapshotDoesNotRequireTsULLGte1_RetryableWrite) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 0), 5);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(RetryImageEnum::kPostImage,
                                        entryOpTime,
                                        boost::none /* commitTimestamp */,
                                        expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPostImageFromSnapshotDoesNotRequireTsULLGte1_TransactionOplogTsEqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 0), 5);
    auto commitTimestamp = Timestamp(0, 0);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPostImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

TEST(FindAndModifyImageLookupTest,
     FetchPostImageFromSnapshotDoesNotRequireTsULLGte1_TransactionOplogTsNeqCommitTxnTs) {
    auto entryOpTime = repl::OpTime(Timestamp(0, 1), 5);
    auto commitTimestamp = Timestamp(0, 0);
    auto expectedAtClusterTime = Timestamp(0, 0);
    testFetchPreOrPostImageFromSnapshot(
        RetryImageEnum::kPostImage, entryOpTime, commitTimestamp, expectedAtClusterTime);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
