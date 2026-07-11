// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/top.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(TopTest, CollectionDropped) {
    Top().collectionDropped(NamespaceString::createNamespaceString_forTest("test.coll"));
}

class TopServiceContextTest : public ServiceContextTest {
public:
    TopServiceContextTest()
        // Need to add a session because latencies are not recorded non-user connections.
        : ServiceContextTest(
              std::make_unique<ScopedGlobalServiceContextForTest>(/*shouldSetupTL=*/false),
              transport::MockSession::create(/*transportLayer=*/nullptr)),
          _opCtx{makeOperationContext()} {}

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(TopServiceContextTest, IncludesEmptyBucketsJustForTotalTime) {
    ServiceLatencyTracker serviceLatencyTracker;
    serviceLatencyTracker.increment(_opCtx.get(),
                                    /*latency=*/Milliseconds(100),
                                    /*workingTime=*/Milliseconds(100),
                                    Command::ReadWriteType::kRead);

    BSONObjBuilder totalTimeBuilder;
    serviceLatencyTracker.appendTotalTimeStats(/*includeHistograms=*/true,
                                               /*slowMSBucketsOnly=*/false,
                                               &totalTimeBuilder);
    BSONObj totalTime = totalTimeBuilder.done();
    BSONObjBuilder workingTimeBuilder;
    serviceLatencyTracker.appendWorkingTimeStats(/*includeHistograms=*/true,
                                                 /*slowMSBucketsOnly=*/false,
                                                 &workingTimeBuilder);
    BSONObj workingTime = workingTimeBuilder.done();

    ASSERT_EQ(totalTime["reads"]["ops"].Long(), 1);
    ASSERT_GT(totalTime["reads"]["histogram"].Obj().nFields(), 1);
    ASSERT_EQ(workingTime["reads"]["ops"].Long(), 1);
    ASSERT_EQ(workingTime["reads"]["histogram"].Obj().nFields(), 1);
}

}  // namespace
