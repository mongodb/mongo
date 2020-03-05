/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class FlowControlTest : public ServiceContextMongoDTest {
public:
    void setUp() {
        ServiceContextMongoDTest::setUp();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        auto replCoordPtr = replCoord.get();
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        FlowControlTicketholder::set(getServiceContext(),
                                     std::make_unique<FlowControlTicketholder>(1000 * 1000));

        // For ease of testing, create a sample on every call.
        gFlowControlSamplePeriod.store(1);
        flowControl = std::make_unique<FlowControl>(replCoordPtr);

        client = getServiceContext()->makeClient("FlowControl Client");
        opCtx = client->makeOperationContext();
    }

    std::unique_ptr<FlowControl> flowControl;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

TEST_F(FlowControlTest, AddingSamples) {
    // Create a sample entry for every five operations. This better simulates reality than the
    // testing value of one. The timestamp is incremented by one for each operation.
    gFlowControlSamplePeriod.store(5);

    int nextTimestamp = 1;

    const auto& samples = flowControl->_getSampledOpsApplied_forTest();
    ASSERT(samples.size() == 0);

    // The first operation will not yet generate a sample.
    flowControl->sample(Timestamp(nextTimestamp++), 1);
    ASSERT(samples.size() == 0);

    // Adding four more entries will generate a new sample.
    for (int idx = 0; idx < 4; ++idx) {
        flowControl->sample(Timestamp(nextTimestamp++), 1);
    }
    ASSERT_EQ(1u, samples.size());
    // Adding five operations in one call will generate a new sample. However, the sampling
    // structure will now have the state:
    //
    // TS: 5 -> 5 operations
    // TS: 6 -> 10 operations
    //
    // In a perfect world, operation 10 would be represented at timestamp 10.
    flowControl->sample(Timestamp(nextTimestamp), 5);
    nextTimestamp += 5;
    ASSERT_EQ(2u, samples.size());

    // Adding nine operations in one call will generate a third sample. Following that with sampling
    // a single operation does not* create a fourth sample. A full five operations must come in to
    // create the next sample.
    flowControl->sample(Timestamp(nextTimestamp), 9);
    nextTimestamp += 9;
    ASSERT_EQ(3u, samples.size());

    flowControl->sample(Timestamp(nextTimestamp++), 1);
    ASSERT_EQ(3u, samples.size());
    flowControl->sample(Timestamp(nextTimestamp), 4);
    ASSERT_EQ(4u, samples.size());
    nextTimestamp += 4;

    ASSERT_EQ(25, nextTimestamp);
    // This test asserts the timestamps in the sample deque. The requirements of those values in
    // practice are very relaxed. It may make sense to remove this testing if the sampling algorithm
    // becomes more sophisticated.
    const bool assertSampledTimestamps = true;
    if (assertSampledTimestamps) {
        ASSERT_EQ(5u, std::get<0>(samples[0]));
        ASSERT_EQ(6u, std::get<0>(samples[1]));
        ASSERT_EQ(11u, std::get<0>(samples[2]));
        ASSERT_EQ(21u, std::get<0>(samples[3]));
    }
}

TEST_F(FlowControlTest, TrimmingSamples) {
    // Create 10 samples from times 0->9.
    for (int idx = 0; idx < 10; ++idx) {
        flowControl->sample(Timestamp(idx), 1);
    }

    const auto& samples = flowControl->_getSampledOpsApplied_forTest();
    ASSERT_EQ(10u, samples.size());

    // Trim all samples smaller than five. This should leave half of the samples.
    flowControl->_trimSamples(Timestamp(5));
    ASSERT_EQ(5u, samples.size());

    // Attempt to trim the remaining samples. Flow control will leave the last two samples alone for
    // calculating other metrics.
    flowControl->_trimSamples(Timestamp(100));
    ASSERT_EQ(2u, samples.size());
}

TEST_F(FlowControlTest, OutOfOrderSamplesDropped) {
    // While operation timestamps are generated in order by replication, they are not given to flow
    // control in order. This helps prevent unnecessary lock contention. Because flow control is
    // resilient to noisy data, it's acceptable to drop data to keep the deque in sorted order (a
    // requirement for searching).
    flowControl->sample(Timestamp(1), 1);
    const auto& samples = flowControl->_getSampledOpsApplied_forTest();
    ASSERT_EQ(1u, samples.size());
    ASSERT_EQ(1u, std::get<0>(samples[0]));

    flowControl->sample(Timestamp(3), 1);
    ASSERT_EQ(2u, samples.size());
    ASSERT_EQ(3u, std::get<0>(samples[1]));

    flowControl->sample(Timestamp(2), 1);
    ASSERT_EQ(2u, samples.size());
    ASSERT_EQ(3u, std::get<0>(samples[1]));
}

TEST_F(FlowControlTest, QueryingSamples) {
    // Create 100 samples from times 0->99.
    for (int idx = 0; idx < 100; ++idx) {
        flowControl->sample(Timestamp(idx), 1);
    }

    for (int start = 0; start < 100; ++start) {
        for (int end = start; end < 100; ++end) {
            ASSERT_EQ(end - start,
                      flowControl->_approximateOpsBetween(Timestamp(start), Timestamp(end)))
                << "Start: " << start << " End: " << end;
        }
    }
}

TEST_F(FlowControlTest, QueryingLocksPerOp) {
    // Create 100 samples. Grab the global IX lock once for the first sample, twice for the second,
    // etc...
    for (int numSamples = 1; numSamples <= 100; ++numSamples) {
        for (int globalLock = 0; globalLock < numSamples; ++globalLock) {
            Lock::GlobalLock lk(opCtx.get(), LockMode::MODE_IX);
        }

        flowControl->sample(Timestamp(numSamples), 1);

        // Need two points to make a line.
        if (numSamples > 1) {
            ASSERT_EQ(numSamples, flowControl->_getLocksPerOp());

            BSONElement noopVar;
            auto serverStatusSection = flowControl->generateSection(opCtx.get(), noopVar);
            ASSERT_EQ(numSamples * 1000, serverStatusSection["locksPerKiloOp"].Double());
        } else {
            ASSERT_EQ(-1.0, flowControl->_getLocksPerOp());
        }
    }
}

TEST_F(FlowControlTest, CalculatingTickets) {
    // Construct a state where the majority point lag is at the threshold with the sustainer node
    // processing 1,000 operations per second. The primary in that case will shoot for 95%
    // (gFlowControlFudgeFactor) of 1,000. Given an input 2.0 locksPerOp, the number of tickets
    // returned should be 950 * 2 = 1900.
    //
    // There's no dependency on gFlowControlDecayConstant in this test because we set the majority
    // point lag to the lag threshold.
    gFlowControlFudgeFactor.store(0.95);

    // Constructs a member data instance with an optime at term 1, timestamp `ts`. The wallclock
    // times are not initialized.
    auto constructMemberData = [](Timestamp ts) -> repl::MemberData {
        repl::MemberData ret;
        ret.setLastAppliedOpTimeAndWallTime({{ts, 1}, Date_t()}, Date_t());
        return ret;
    };

    // In the previous observation, all nodes are applied up through 1000.
    std::vector<repl::MemberData> prevMemberData;
    prevMemberData.emplace_back(constructMemberData(Timestamp(1000)));
    prevMemberData.emplace_back(constructMemberData(Timestamp(1000)));
    prevMemberData.emplace_back(constructMemberData(Timestamp(1000)));

    // In the current observation, the secondaries are at 2000 while the primary is at 3000.
    std::vector<repl::MemberData> currMemberData;
    currMemberData.emplace_back(constructMemberData(Timestamp(2000)));
    currMemberData.emplace_back(constructMemberData(Timestamp(2000)));
    currMemberData.emplace_back(constructMemberData(Timestamp(3000)));

    // Construct samples where Timestamp X maps to operation number X.
    for (int ts = 1; ts <= 3000; ++ts) {
        flowControl->sample(Timestamp(ts), 1);
    }

    const std::int64_t locksUsedLastPeriod = -1;  // Irrelevant to this call.
    const double locksPerOp = 2.0;
    const std::uint64_t thresholdLag = 1;
    const std::uint64_t currLag = thresholdLag;
    ASSERT_EQ(1900,
              flowControl->_calculateNewTicketsForLag(prevMemberData,
                                                      currMemberData,
                                                      locksUsedLastPeriod,
                                                      locksPerOp,
                                                      currLag,
                                                      thresholdLag));
}
}  // namespace mongo
