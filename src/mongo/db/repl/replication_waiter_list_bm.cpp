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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/replication_coordinator_test_fixture.h"
#include "mongo/db/repl/storage_interface_mock.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {

// This is re-using our test harness to do benchmarking.
class WaiterBM : public ReplCoordTest {
public:
    void setUp() override {
        ReplCoordTest::setUp();
    }
    void tearDown() override {
        ReplCoordTest::tearDown();
    }
    BSONObj createConfig(int nnodes) {
        BSONObjBuilder bob;
        bob.append("_id", "mySet");
        bob.append("version", 1);
        BSONArrayBuilder mbob(bob.subarrayStart("members"));
        for (int i = 1; i <= nnodes; i++) {
            std::string nodestr = std::string("node") + std::to_string(i) + ":12345";
            mbob.append(BSON("_id" << i << "host" << nodestr));
        }
        mbob.done();
        bob.append("protocolVersion", 1);
        return bob.obj();
    }
    enum Orderings {
        kOrderingSame,
        kOrderingSequential,
        // kOrderingScrambled is intended to test inserting in a non-sequential order; it is not
        // actually implemented.
        kOrderingScrambled,
    };

    // Benchmarks adding write concern majority waiters to the replication waiter list.
    void benchmarkAddingWaiters(benchmark::State& state) {
        const int nMembers = 3;  // shouldn't matter.
        int nWaiters = Orderings(state.range(0));
        Orderings orderType = Orderings(state.range(1));
        auto configObj = createConfig(nMembers);
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        auto* replCoord = getReplCoord();
        OpTime startOpTime{{100, 1}, 1};
        // Become primary
        ASSERT_FALSE(replCoord->getMemberState().secondary());
        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(startOpTime, Date_t::now());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        simulateSuccessfulV1Election();
        ASSERT(replCoord->getMemberState().primary());

        auto wcMajority = WriteConcernOptions::parse(WriteConcernOptions::Majority).getValue();
        OpTime waitOpTime = OpTime{startOpTime.getTimestamp() + 1, 1};
        std::vector<SemiFuture<void>> futures;
        futures.reserve(nWaiters);
        for (auto _ : state) {
            for (int i = 0; i < nWaiters; i++) {
                futures.emplace_back(
                    replCoord->awaitReplicationAsyncNoWTimeout(waitOpTime, wcMajority).semi());
                if (orderType == kOrderingSequential) {
                    waitOpTime = OpTime{waitOpTime.getTimestamp() + 1, 1};
                }
            }
            state.PauseTiming();
            replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(waitOpTime, Date_t::now());
            getStorageInterface()->allDurableTimestamp = waitOpTime.getTimestamp();
            ASSERT_OK(replCoord->setLastWrittenOptime_forTest(1, 2, waitOpTime));
            ASSERT_OK(replCoord->setLastAppliedOptime_forTest(1, 2, waitOpTime));
            ASSERT_OK(replCoord->setLastDurableOptime_forTest(1, 2, waitOpTime));
            futures.clear();
            waitOpTime = OpTime{waitOpTime.getTimestamp() + 1, 1};
            state.ResumeTiming();
        }
    }

    // Benchmarks advancing opTimes when there a number of {w:all} waiters which aren't fulfilled
    // by the new optimes.
    void benchmarkUnfulfilledWaiters(benchmark::State& state) {
        int nMembers = state.range(0);
        // This test is not valid with one member.
        ASSERT_GT(nMembers, 1);
        int nUnfulfilled = state.range(1);
        auto configObj = createConfig(nMembers);
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        auto* replCoord = getReplCoord();
        OpTime startOpTime{{100, 1}, 1};
        // Become primary
        ASSERT_FALSE(replCoord->getMemberState().secondary());
        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(startOpTime, Date_t::now());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        simulateSuccessfulV1Election();
        ASSERT(replCoord->getMemberState().primary());

        auto wcAll = WriteConcernOptions(
            nMembers, WriteConcernOptions::SyncMode::JOURNAL, Milliseconds(0) /* timeout */);
        OpTime fulfilledOpTime = OpTime{{startOpTime.getTimestamp().getSecs() + 10, 1}, 1};
        for (auto _ : state) {
            state.PauseTiming();
            std::vector<SemiFuture<void>> futures;
            for (int i = 0; i < nUnfulfilled; i++) {
                auto future = replCoord->awaitReplicationAsyncNoWTimeout(fulfilledOpTime, wcAll);
                futures.emplace_back(std::move(future).semi());
            }
            state.ResumeTiming();
            getStorageInterface()->allDurableTimestamp = fulfilledOpTime.getTimestamp();
            replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(fulfilledOpTime, Date_t::now());
            for (int i = 2; i < nMembers; i++) {
                ASSERT_OK(replCoord->setLastWrittenOptime_forTest(1, i, fulfilledOpTime));
                ASSERT_OK(replCoord->setLastAppliedOptime_forTest(1, i, fulfilledOpTime));
                ASSERT_OK(replCoord->setLastDurableOptime_forTest(1, i, fulfilledOpTime));
            }
            // Need to fulfill last member to clear lists.
            state.PauseTiming();
            ASSERT_OK(replCoord->setLastWrittenOptime_forTest(1, nMembers, fulfilledOpTime));
            ASSERT_OK(replCoord->setLastAppliedOptime_forTest(1, nMembers, fulfilledOpTime));
            ASSERT_OK(replCoord->setLastDurableOptime_forTest(1, nMembers, fulfilledOpTime));
            fulfilledOpTime = OpTime{fulfilledOpTime.getTimestamp() + 1, 1};
            // Make sure we don't overflow.
            invariant(!(fulfilledOpTime.getTimestamp().getInc() & 0x8000000));
            // Destroy futures array while timing is paused
            futures = std::vector<SemiFuture<void>>();
            state.ResumeTiming();
        }
    }

    // Benchmarks advancing opTimes when there a number of {w:"majority"} waiters which aren't
    // fulfilled by the new optimes.
    void benchmarkUnfulfilledMajorityWaiters(benchmark::State& state) {
        int nMembers = state.range(0);
        // This test is not valid with one member.
        ASSERT_GT(nMembers, 1);
        int nUnfulfilled = state.range(1);
        auto configObj = createConfig(nMembers);
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
        auto* replCoord = getReplCoord();
        OpTime startOpTime{{100, 1}, 1};
        // Become primary
        ASSERT_FALSE(replCoord->getMemberState().secondary());
        replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(startOpTime, Date_t::now());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));
        simulateSuccessfulV1Election();
        ASSERT(replCoord->getMemberState().primary());

        auto wcMajority = WriteConcernOptions::parse(WriteConcernOptions::Majority).getValue();
        OpTime fulfilledOpTime = OpTime{{startOpTime.getTimestamp().getSecs() + 10, 1}, 1};
        for (auto _ : state) {
            state.PauseTiming();
            std::vector<SemiFuture<void>> futures;
            for (int i = 0; i < nUnfulfilled; i++) {
                auto future =
                    replCoord->awaitReplicationAsyncNoWTimeout(fulfilledOpTime, wcMajority);
                futures.emplace_back(std::move(future).semi());
            }
            state.ResumeTiming();
            getStorageInterface()->allDurableTimestamp = fulfilledOpTime.getTimestamp();
            replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(fulfilledOpTime, Date_t::now());
            int lastMajorityMember = (nMembers + 1) / 2;
            for (int i = 2; i < lastMajorityMember; i++) {
                ASSERT_OK(replCoord->setLastWrittenOptime_forTest(1, i, fulfilledOpTime));
                ASSERT_OK(replCoord->setLastAppliedOptime_forTest(1, i, fulfilledOpTime));
                ASSERT_OK(replCoord->setLastDurableOptime_forTest(1, i, fulfilledOpTime));
            }
            state.PauseTiming();
            // Need to fulfill one more member to clear lists.
            ASSERT_FALSE(futures[0].isReady());
            ASSERT_OK(replCoord->setLastWrittenOptime_forTest(1, nMembers, fulfilledOpTime));
            ASSERT_OK(replCoord->setLastAppliedOptime_forTest(1, nMembers, fulfilledOpTime));
            ASSERT_OK(replCoord->setLastDurableOptime_forTest(1, nMembers, fulfilledOpTime));
            fulfilledOpTime = OpTime{fulfilledOpTime.getTimestamp() + 1, 1};
            // Make sure we don't overflow.
            invariant(!(fulfilledOpTime.getTimestamp().getInc() & 0x8000000));
            // Dispose of the futures outside the timing.
            futures = std::vector<SemiFuture<void>>();
            state.ResumeTiming();
        }
    }

    void startCoordForConfigBM() {
        BSONObj configObj = BSON("_id" << "mySet"
                                       << "version" << 1 << "members"
                                       << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                << "node1:12345")
                                                     << BSON("_id" << 2 << "host"
                                                                   << "node2:12345"))
                                       << "protocolVersion" << 1);
        assertStartSuccess(configObj, HostAndPort("node1", 12345));
    }

    void _doTest() final {}
};

namespace {
struct TestSuiteEnvironment {
    // This is boilerplate to match the unit test framework, which is re-purposed here to run
    // benchmarks.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    explicit TestSuiteEnvironment() {
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    }
    ~TestSuiteEnvironment() {
        serverGlobalParams.mutableFCV.reset();
    }
};
}  // namespace

void BM_UnfulfilledWaiter(benchmark::State& state) {
    TestSuiteEnvironment env;
    WaiterBM bm;
    bm.setUp();
    bm.benchmarkUnfulfilledWaiters(state);
    bm.tearDown();
}

void BM_UnfulfilledMajorityWaiter(benchmark::State& state) {
    TestSuiteEnvironment env;
    WaiterBM bm;
    bm.setUp();
    bm.benchmarkUnfulfilledMajorityWaiters(state);
    bm.tearDown();
}

void BM_AddWaiter(benchmark::State& state) {
    TestSuiteEnvironment env;
    WaiterBM bm;
    bm.setUp();
    bm.benchmarkAddingWaiters(state);
    bm.tearDown();
}

BENCHMARK(BM_UnfulfilledWaiter)->Args({3, 1000})->MinTime(0.05);
BENCHMARK(BM_UnfulfilledMajorityWaiter)->Args({3, 1000})->MinTime(0.05);
BENCHMARK(BM_AddWaiter)->Args({1000, WaiterBM::kOrderingSequential})->MinTime(0.05);
}  // namespace repl
}  // namespace mongo
