// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/active_index_builds.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/index_builds/repl_index_build_state.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const std::vector<double> kHistogramBucketBoundaries = {
    10,           // 10ms
    1'000,        // 1s
    10'000,       // 10s
    60'000,       // 1min
    300'000,      // 5min
    1'800'000,    // 30min
    7'200'000,    // 2h
    28'800'000,   // 8h
    86'400'000,   // 1d
    172'800'000,  // 2d
    259'200'000,  // 3d
    432'000'000,  // 5d
};
const size_t kNumHistogramBuckets = kHistogramBucketBoundaries.size() + 1;

constexpr IndexBuildPhaseEnum kAllPhases[] = {IndexBuildPhaseEnum::kInitialized,
                                              IndexBuildPhaseEnum::kCollectionScan,
                                              IndexBuildPhaseEnum::kBulkLoad,
                                              IndexBuildPhaseEnum::kDrainWrites};

constexpr IndexBuildOutcome kAllOutcomes[] = {
    IndexBuildOutcome::kSuccess, IndexBuildOutcome::kFailure, IndexBuildOutcome::kToBeResumed};

class IndexBuildsDurationTest : public ServiceContextTest {
public:
    void SetUp() override {
        ServiceContextTest::SetUp();
        if (!_capturer.canReadMetrics()) {
            GTEST_SKIP() << "Skipping test due to OTel metrics being unavailable in this build";
        }
    }

protected:
    /**
     * Reads the histogram data for a given combination of start phase and outcome.
     */
    otel::metrics::HistogramData<int64_t> readDuration(IndexBuildPhaseEnum startPhase,
                                                       IndexBuildOutcome outcome) {
        return _capturer.readInt64Histogram(
            otel::metrics::MetricNames::kIndexBuildCompletedDurationMillis,
            std::tuple{idl::serialize(startPhase), toString(outcome)});
    }

    /**
     * Builds the state that the index builds coordinator would register for a build beginning at
     * 'startTime'.
     */
    std::shared_ptr<ReplIndexBuildState> makeReplState(Date_t startTime) {
        return std::make_shared<ReplIndexBuildState>(
            UUID::gen(),
            UUID::gen(),
            DatabaseName::createDatabaseName_forTest(boost::none, "test"),
            std::vector<IndexBuildInfo>{},
            IndexBuildProtocol::kTwoPhase,
            startTime);
    }

    /**
     * Simulates running an index build with the given start phase and outcome, running for
     * 'duration' milliseconds.
     */
    void runIndexBuild(IndexBuildPhaseEnum startPhase,
                       IndexBuildOutcome outcome,
                       Milliseconds duration) {
        auto replState = makeReplState(Date_t::now() - duration);

        if (startPhase != IndexBuildPhaseEnum::kInitialized) {
            replState->setIndexBuildStartPhase(startPhase);
        }

        ASSERT_OK(_activeIndexBuilds.registerIndexBuild(replState));
        _activeIndexBuilds.unregisterIndexBuild(&_indexBuildsManager, replState, outcome);
    }

    otel::metrics::OtelMetricsCapturer _capturer;
    IndexBuildsManager _indexBuildsManager;
    ActiveIndexBuilds _activeIndexBuilds;
};

/*
 * A test case for the ActiveIndexBuildsDurationTest parameterized test. Includes the start phase
 * and outcome of the index build, the duration that a build ran for, and the expected histogram
 * bucket that the duration is expected to be recorded in.
 */
struct DurationTestCase {
    IndexBuildPhaseEnum startPhase;
    IndexBuildOutcome outcome;
    Milliseconds duration;
    size_t expectedBucket;
};

class IndexBuildsDurationHistogramTest : public IndexBuildsDurationTest,
                                         public testing::WithParamInterface<DurationTestCase> {};

TEST_P(IndexBuildsDurationHistogramTest, RecordsDurationByStartPhaseAndOutcome) {
    const auto& testCase = GetParam();

    runIndexBuild(testCase.startPhase, testCase.outcome, testCase.duration);

    const auto data = readDuration(testCase.startPhase, testCase.outcome);
    EXPECT_EQ(data.count, 1UL);
    // Assert that the duration of the index build was at least as long as the provided duration,
    // with some wiggle room. An upper bound on the wiggle room is provided by the check below that
    // only the expected histogram bucket was recorded to and not any other.
    EXPECT_GE(data.sum, testCase.duration.count());

    ASSERT_EQ(data.counts.size(), kNumHistogramBuckets);
    for (size_t bucket = 0; bucket < data.counts.size(); ++bucket) {
        EXPECT_EQ(data.counts[bucket], bucket == testCase.expectedBucket ? 1UL : 0UL)
            << "bucket " << bucket;
    }
}

std::string durationTestCaseName(const testing::TestParamInfo<DurationTestCase>& info) {
    std::string name{idl::serialize(info.param.startPhase)};
    name += "_";
    name += toString(info.param.outcome);
    name += "_bucket";
    name += std::to_string(info.param.expectedBucket);
    std::replace(name.begin(), name.end(), ' ', '_');
    return name;
}

std::vector<DurationTestCase> makeDurationTestCases() {
    std::vector<DurationTestCase> testCases;
    for (auto startPhase : kAllPhases) {
        for (auto outcome : kAllOutcomes) {
            testCases.push_back({startPhase, outcome, Milliseconds(0), 0});    // (0, 10ms]
            testCases.push_back({startPhase, outcome, Milliseconds(500), 1});  // (10ms, 1000ms]
            testCases.push_back(
                {startPhase, outcome, Milliseconds(5'000), 2});  // (1000ms, 10000ms]
        }
    }
    return testCases;
}

INSTANTIATE_TEST_SUITE_P(AllStartPhasesAndOutcomes,
                         IndexBuildsDurationHistogramTest,
                         testing::ValuesIn(makeDurationTestCases()),
                         durationTestCaseName);

TEST_F(IndexBuildsDurationTest, UsesExplicitBucketBoundaries) {
    runIndexBuild(IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kSuccess, Milliseconds(0));

    const auto data = readDuration(IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kSuccess);
    EXPECT_EQ(data.boundaries, kHistogramBucketBoundaries);
}

TEST_F(IndexBuildsDurationTest, DefaultsToInitializedStartPhase) {
    // A build that is never resumed leaves 'setIndexBuildStartPhase' uncalled.
    runIndexBuild(IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kSuccess, Milliseconds(0));

    EXPECT_EQ(readDuration(IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kSuccess).count,
              1UL);
}

TEST_F(IndexBuildsDurationTest, ClampsDurationWhenStartTimeIsInTheFuture) {
    // The start time is taken from the wall clock, which can move backwards. The histogram rejects
    // negative values, so the duration must be clamped rather than allowed to throw.
    runIndexBuild(
        IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kFailure, Milliseconds(-60'000));

    const auto data = readDuration(IndexBuildPhaseEnum::kInitialized, IndexBuildOutcome::kFailure);
    EXPECT_EQ(data.count, 1UL);
    EXPECT_EQ(data.sum, 0);
    EXPECT_EQ(data.counts[0], 1UL);
}

}  // namespace
}  // namespace mongo
