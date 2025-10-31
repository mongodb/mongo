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

#include "mongo/db/stats/top.h"

#include "mongo/base/string_data.h"
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
    ASSERT_GT(totalTime["reads"]["histogram"].Array().size(), 1);
    ASSERT_EQ(workingTime["reads"]["ops"].Long(), 1);
    ASSERT_EQ(workingTime["reads"]["histogram"].Array().size(), 1);
}

}  // namespace
