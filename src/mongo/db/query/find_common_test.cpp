// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/find_common.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace {

using namespace mongo;

TEST(BSONArrayResponseSizeTrackerTest, AddLargeNumberOfElements) {
    BSONObjBuilder bsonObjBuilder;
    {
        FindCommon::BSONArrayResponseSizeTracker sizeTracker;
        BSONArrayBuilder arrayBuilder{bsonObjBuilder.subarrayStart("a")};
        BSONObj emptyObject;
        while (sizeTracker.haveSpaceForNext(emptyObject)) {
            sizeTracker.add(emptyObject);
            arrayBuilder.append(emptyObject);
        }
    }
    // If the BSON object is successfully constructed, then space accounting was correct.
    bsonObjBuilder.obj();
}
TEST(BSONArrayResponseSizeTrackerTest, CanAddAtLeastOneDocument) {
    auto largeObject = BSON("a" << std::string(16 * 1024 * 1024, 'A'));
    BSONObj emptyObject;
    BSONObjBuilder bsonObjBuilder;
    {
        FindCommon::BSONArrayResponseSizeTracker sizeTracker;
        BSONArrayBuilder arrayBuilder{bsonObjBuilder.subarrayStart("a")};
        // Add an object that is larger than 16MB.
        ASSERT(sizeTracker.haveSpaceForNext(largeObject));
        sizeTracker.add(largeObject);
        arrayBuilder.append(largeObject);
        ASSERT(!sizeTracker.haveSpaceForNext(emptyObject));
    }
    // If the BSON object is successfully constructed, then space accounting was correct.
    bsonObjBuilder.obj();
}

TEST(BSONObjCursorAppenderTest, FailsToAppendWhenPBRTDoesNotFit) {
    // Tests that even with a large post-batch resume token, a cursor response (a.k.a. a batch)
    // still fits into 16MB.

    const size_t doc2ApproxSize = 80 * 1024;  // Not large, but not small;
    std::string largeString(doc2ApproxSize, 'x');
    std::string veryLargeString(FindCommon::kMaxBytesToReturnToClientAtOnce - 2.5 * doc2ApproxSize,
                                'x');

    BSONObj doc1 = BSON("a" << veryLargeString);
    BSONObj doc2 = BSON("_id" << largeString);
    // We can create a real resume token from 'doc2', but it is unnecessary here. We just need a
    // large enough BSON object.
    BSONObj resumeToken = doc2;

    rpc::OpMsgReplyBuilder reply;
    CursorResponseBuilder::Options options;
    CursorResponseBuilder nextBatch(&reply, options);

    // Append 'doc1' to the cursor response.
    nextBatch.append(doc1);
    // There should be enough space for the resume token.
    ASSERT(FindCommon::fitsInBatch(nextBatch.bytesUsed(), resumeToken.objsize()));

    bool failedToAppend = false;
    FindCommon::BSONObjCursorAppender appenderFn{
        false /* alwaysAcceptFirstDoc */, &nextBatch, resumeToken, failedToAppend};

    uint64_t numResults = 1;

    // Append 'doc2' to the cursor response using the appender function. Appending should succeed.
    const bool append1Result = appenderFn(doc2, resumeToken, numResults);
    ASSERT_FALSE(failedToAppend);
    ASSERT_TRUE(append1Result);
    // There should be enough space for the resume token.
    ASSERT(FindCommon::fitsInBatch(nextBatch.bytesUsed(), resumeToken.objsize()));

    numResults++;

    // Append 'doc2' once more using the appender function. Appending should fail.
    const bool append2Result = appenderFn(doc2, resumeToken, numResults);
    ASSERT_TRUE(failedToAppend);
    ASSERT_FALSE(append2Result);
    // There should be enough space for the resume token.
    ASSERT(FindCommon::fitsInBatch(nextBatch.bytesUsed(), resumeToken.objsize()));
}
}  // namespace
