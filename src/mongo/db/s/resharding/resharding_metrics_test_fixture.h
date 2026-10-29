// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/static_immortal.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class ObserverMock : public ReshardingMetricsObserver {
public:
    constexpr static auto kDefaultRole = ReshardingMetricsCommon::Role::kCoordinator;
    ObserverMock(Date_t startTime, int64_t timeRemaining)
        : ObserverMock{startTime, timeRemaining, timeRemaining, kDefaultRole} {}
    ObserverMock(Date_t startTime,
                 int64_t timeRemainingHigh,
                 int64_t timeRemainingLow,
                 ReshardingMetricsCommon::Role role)
        : _uuid{UUID::gen()},
          _startTime{startTime},
          _timeRemainingHigh{timeRemainingHigh},
          _timeRemainingLow{timeRemainingLow},
          _role{role} {
        invariant(timeRemainingHigh >= timeRemainingLow);
    }

    const UUID& getUuid() const override {
        return _uuid;
    }

    boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis() const override {
        return _timeRemainingHigh;
    }

    boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const override {
        return _timeRemainingLow;
    }

    Date_t getStartTimestamp() const override {
        return _startTime;
    }

    ReshardingMetricsCommon::Role getRole() const override {
        return _role;
    }

    BSONObj getDiagnosticMetrics() const override {
        return ReshardingMetrics::getDiagnosticMetricDefaults(_role);
    }

private:
    UUID _uuid;
    Date_t _startTime;
    Milliseconds _timeRemainingHigh;
    Milliseconds _timeRemainingLow;
    ReshardingMetricsCommon::Role _role;
};

class ReshardingMetricsTestFixture : public unittest::Test {

public:
    using Role = ReshardingMetricsCommon::Role;
    template <typename T>
    void runTimeReportTest(const std::string& testName,
                           const std::initializer_list<Role>& roles,
                           const std::string& timeField,
                           const std::function<void(T*)>& beginTimedSection,
                           const std::function<void(T*)>& endTimedSection) {
        constexpr auto kIncrement = Milliseconds(5000);
        const auto kIncrementInSeconds = durationCount<Seconds>(kIncrement);
        for (const auto& role : roles) {
            LOGV2(6437400, "", "TestName"_attr = testName, "Role"_attr = role);
            auto uuid = UUID::gen();
            const auto& clock = getClockSource();
            auto metrics = std::make_unique<T>(uuid,
                                               kTestCommand,
                                               kTestNamespace,
                                               role,
                                               clock->now(),
                                               clock,
                                               _cumulativeMetrics.get(),
                                               ReshardingMetrics::getDefaultState(role),
                                               ReshardingProvenanceEnum::kReshardCollection);

            // Reports 0 before timed section entered.
            clock->advance(kIncrement);
            auto report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), 0);

            // Reports time so far during critical section.
            beginTimedSection(metrics.get());
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds);
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds * 2);

            // Still reports total time after critical section ends.
            endTimedSection(metrics.get());
            clock->advance(kIncrement);
            report = metrics->reportForCurrentOp();
            ASSERT_EQ(report.getIntField(timeField), kIncrementInSeconds * 2);
        }
    }

protected:
    void setUp() override {
        _cumulativeMetrics = initializeCumulativeMetrics();
    }

    virtual std::string_view getRootSectionName() {
        return kTestMetricsName;
    }

    enum Section { kRoot, kActive, kLatencies, kCurrentInSteps };

    std::string_view getSectionName(Section section) {
        switch (section) {
            case kRoot:
                return getRootSectionName();
            case kActive:
                return "active";
            case kLatencies:
                return "latencies";
            case kCurrentInSteps:
                return "currentInSteps";
        }
        MONGO_UNREACHABLE;
    }

    BSONObj getCumulativeMetricsReport() {
        BSONObjBuilder bob;
        getCumulativeMetrics()->reportForServerStatus(&bob);
        return bob.obj().getObjectField(getSectionName(kRoot)).getOwned();
    }

    BSONObj getReportSection(BSONObj report, Section section) {
        if (section == kRoot) {
            return report.getOwned();
        }
        return report.getObjectField(getSectionName(section)).getOwned();
    }

    BSONObj getCumulativeMetricsReportForSection(Section section) {
        return getReportSection(getCumulativeMetricsReport(), section);
    }

    void assertAltersCumulativeMetrics(ReshardingMetrics* metrics,
                                       const std::function<void(ReshardingMetrics*)>& mutateFn,
                                       const std::function<bool(BSONObj, BSONObj)>& verifyFn) {
        auto before = getCumulativeMetricsReport();
        mutateFn(metrics);
        auto after = getCumulativeMetricsReport();
        ASSERT_TRUE(verifyFn(before, after));
    }

    void assertAltersCumulativeMetricsField(ReshardingMetrics* metrics,
                                            const std::function<void(ReshardingMetrics*)>& mutateFn,
                                            Section section,
                                            std::string_view fieldName,
                                            const std::function<bool(int, int)>& verifyFn) {
        assertAltersCumulativeMetrics(metrics, mutateFn, [&](auto reportBefore, auto reportAfter) {
            auto before = getReportSection(reportBefore, section).getIntField(fieldName);
            auto after = getReportSection(reportAfter, section).getIntField(fieldName);
            return verifyFn(before, after);
        });
    }

    void assertIncrementsCumulativeMetricsField(
        ReshardingMetrics* metrics,
        const std::function<void(ReshardingMetrics*)>& mutateFn,
        Section section,
        std::string_view fieldName) {
        assertAltersCumulativeMetricsField(
            metrics, mutateFn, section, fieldName, [](auto before, auto after) {
                return after > before;
            });
    }

    void assertDecrementsCumulativeMetricsField(
        ReshardingMetrics* metrics,
        const std::function<void(ReshardingMetrics*)>& mutateFn,
        Section section,
        std::string_view fieldName) {
        assertAltersCumulativeMetricsField(
            metrics, mutateFn, section, fieldName, [](auto before, auto after) {
                return after < before;
            });
    }

    constexpr static auto kTestMetricsName = "testMetrics";
    constexpr static auto kYoungestTime =
        Date_t::fromMillisSinceEpoch(std::numeric_limits<int64_t>::max());
    constexpr static int64_t kYoungestTimeLeft = 5000;
    constexpr static auto kOldestTime =
        Date_t::fromMillisSinceEpoch(std::numeric_limits<int64_t>::min());
    constexpr static int64_t kOldestTimeLeft = 3000;
    const NamespaceString kTestNamespace =
        NamespaceString::createNamespaceString_forTest("test.source");
    const BSONObj kTestCommand = BSON("command" << "test");

    std::unique_ptr<ReshardingCumulativeMetrics> initializeCumulativeMetrics() {
        return std::make_unique<ReshardingCumulativeMetrics>();
    }

    const ObserverMock* getYoungestObserver() {
        static StaticImmortal<ObserverMock> youngest{kYoungestTime, kYoungestTimeLeft};
        return &youngest.value();
    }

    const ObserverMock* getOldestObserver() {
        static StaticImmortal<ObserverMock> oldest{kOldestTime, kOldestTimeLeft};
        return &oldest.value();
    }

    ClockSourceMock* getClockSource() {
        static StaticImmortal<ClockSourceMock> clock;
        return &clock.value();
    }

    ReshardingCumulativeMetrics* getCumulativeMetrics() {
        return _cumulativeMetrics.get();
    }

    using SpecialIndexBehaviorMap = stdx::unordered_map<int, std::function<void()>>;
    const SpecialIndexBehaviorMap kNoSpecialBehavior{};
    SpecialIndexBehaviorMap registerAtIndex(int index, const ObserverMock* mock) {
        return SpecialIndexBehaviorMap{{index, [this, mock] {
                                            _observers.emplace_back(
                                                _cumulativeMetrics->registerInstanceMetrics(mock));
                                        }}};
    }

    template <typename ScopedObserverType>
    void performRandomOperations(std::vector<std::unique_ptr<ScopedObserverType>>& inserted,
                                 const int iterations,
                                 const float removalOdds,
                                 const int64_t seed,
                                 const SpecialIndexBehaviorMap& specialBehaviors) {
        constexpr auto kThresholdScale = 1000;
        const auto kRemovalThreshold = kThresholdScale * removalOdds;
        PseudoRandom rng(seed);
        auto shouldPerformRemoval = [&] {
            return (rng.nextInt32(kThresholdScale)) < kRemovalThreshold;
        };
        auto performInsertion = [&] {
            auto timeLeft = rng.nextInt64(std::numeric_limits<int64_t>::max());
            auto startTime = rng.nextInt64();
            startTime = (startTime == kOldestTime.asInt64()) ? startTime + 1 : startTime;
            inserted.emplace_back(
                std::make_unique<ScopedObserverType>(Date_t::fromMillisSinceEpoch(startTime),
                                                     timeLeft,
                                                     getClockSource(),
                                                     _cumulativeMetrics.get()));
        };
        auto performRemoval = [&] {
            auto i = rng.nextInt32(inserted.size());
            inserted.erase(inserted.begin() + i);
        };
        for (auto i = 0; i < iterations; i++) {
            auto it = specialBehaviors.find(i);
            if (it != specialBehaviors.end()) {
                it->second();
                continue;
            }
            if (!inserted.empty() && shouldPerformRemoval()) {
                performRemoval();
            } else {
                performInsertion();
            }
        }
    }

    template <typename ScopedObserverType>
    void doRandomOperationsTest() {
        constexpr auto kIterations = 10000;
        constexpr auto kRemovalOdds = 0.10f;
        const auto seed = SecureRandom().nextInt64();
        LOGV2(6315200, "StillReportsOldestAfterRandomOperations", "seed"_attr = seed);
        PseudoRandom rng(seed);
        std::vector<std::unique_ptr<ScopedObserverType>> inserted;
        performRandomOperations(inserted,
                                kIterations,
                                kRemovalOdds,
                                rng.nextInt64(),
                                registerAtIndex(rng.nextInt32(kIterations), getOldestObserver()));
        ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                      ObserverMock::kDefaultRole),
                  kOldestTimeLeft);
    }

    template <typename ScopedObserverType>
    void doRandomOperationsMultithreadedTest() {
        constexpr auto kIterations = 10000;
        constexpr auto kRemovalOdds = 0.10f;
        constexpr auto kThreadCount = 10;
        const auto seed = SecureRandom().nextInt64();
        LOGV2(6315201, "StillReportsOldestAfterRandomOperationsMultithreaded", "seed"_attr = seed);
        PseudoRandom rng(seed);
        const auto threadToInsertOldest = rng.nextInt32(kThreadCount);
        std::vector<std::vector<std::unique_ptr<ScopedObserverType>>> threadStorage(kThreadCount);
        std::vector<PromiseAndFuture<void>> threadPFs;
        for (auto i = 0; i < kThreadCount; i++) {
            threadPFs.emplace_back(makePromiseFuture<void>());
        }
        std::vector<stdx::thread> threads;
        for (auto i = 0; i < kThreadCount; i++) {
            auto& storage = threadStorage[i];
            auto seed = rng.nextInt64();
            auto specialBehavior = (i == threadToInsertOldest)
                ? registerAtIndex(rng.nextInt32(kIterations), getOldestObserver())
                : kNoSpecialBehavior;
            auto& done = threadPFs[i].promise;
            threads.emplace_back(
                [=, this, &storage, specialBehavior = std::move(specialBehavior), &done] {
                    performRandomOperations(
                        storage, kIterations, kRemovalOdds, seed, specialBehavior);
                    done.emplaceValue();
                });
        }
        for (auto& pf : threadPFs) {
            pf.future.wait();
        }
        ASSERT_EQ(_cumulativeMetrics->getOldestOperationHighEstimateRemainingTimeMillis(
                      ObserverMock::kDefaultRole),
                  kOldestTimeLeft);
        size_t expectedCount = 1;  // Special insert for kOldest is not counted in vector size.
        for (auto& v : threadStorage) {
            expectedCount += v.size();
        }
        ASSERT_EQ(_cumulativeMetrics->getObservedMetricsCount(), expectedCount);
        for (auto& t : threads) {
            t.join();
        }
    }

    std::unique_ptr<ReshardingCumulativeMetrics> _cumulativeMetrics;
    std::vector<ReshardingCumulativeMetrics::UniqueScopedObserver> _observers;
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
