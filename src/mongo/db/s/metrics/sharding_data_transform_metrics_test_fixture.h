/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class ObserverMock : public ShardingDataTransformMetricsObserverInterface {
public:
    constexpr static auto kDefaultRole = ShardingDataTransformMetrics::Role::kCoordinator;
    ObserverMock(Date_t startTime, int64_t timeRemaining)
        : ObserverMock{startTime, timeRemaining, timeRemaining, kDefaultRole} {}
    ObserverMock(Date_t startTime,
                 int64_t timeRemainingHigh,
                 int64_t timeRemainingLow,
                 ShardingDataTransformMetrics::Role role)
        : _uuid{UUID::gen()},
          _startTime{startTime},
          _timeRemainingHigh{timeRemainingHigh},
          _timeRemainingLow{timeRemainingLow},
          _role{role} {
        invariant(timeRemainingHigh >= timeRemainingLow);
    }

    virtual const UUID& getUuid() const override {
        return _uuid;
    }

    virtual boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis() const override {
        return _timeRemainingHigh;
    }

    virtual boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const override {
        return _timeRemainingLow;
    }

    virtual Date_t getStartTimestamp() const override {
        return _startTime;
    }

    virtual ShardingDataTransformMetrics::Role getRole() const override {
        return _role;
    }

private:
    UUID _uuid;
    Date_t _startTime;
    Milliseconds _timeRemainingHigh;
    Milliseconds _timeRemainingLow;
    ShardingDataTransformMetrics::Role _role;
};

class ShardingDataTransformCumulativeMetricsFieldNameProviderForTest
    : public ShardingDataTransformCumulativeMetricsFieldNameProvider {
public:
    virtual ~ShardingDataTransformCumulativeMetricsFieldNameProviderForTest() = default;
    virtual StringData getForDocumentsProcessed() const override {
        return "documentsProcessed";
    }
    virtual StringData getForBytesWritten() const override {
        return "bytesWritten";
    }
};

class ShardingDataTransformMetricsTestFixture : public unittest::Test {

public:
    using Role = ShardingDataTransformMetrics::Role;
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
                                               _cumulativeMetrics.get());

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

    virtual StringData getRootSectionName() {
        return kTestMetricsName;
    }

    enum Section { kRoot, kActive, kLatencies, kCurrentInSteps };

    StringData getSectionName(Section section) {
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

    void assertAltersCumulativeMetrics(
        ShardingDataTransformInstanceMetrics* metrics,
        const std::function<void(ShardingDataTransformInstanceMetrics*)>& mutateFn,
        const std::function<bool(BSONObj, BSONObj)>& verifyFn) {
        auto before = getCumulativeMetricsReport();
        mutateFn(metrics);
        auto after = getCumulativeMetricsReport();
        ASSERT_TRUE(verifyFn(before, after));
    }

    void assertAltersCumulativeMetricsField(
        ShardingDataTransformInstanceMetrics* metrics,
        const std::function<void(ShardingDataTransformInstanceMetrics*)>& mutateFn,
        Section section,
        StringData fieldName,
        const std::function<bool(int, int)>& verifyFn) {
        assertAltersCumulativeMetrics(metrics, mutateFn, [&](auto reportBefore, auto reportAfter) {
            auto before = getReportSection(reportBefore, section).getIntField(fieldName);
            auto after = getReportSection(reportAfter, section).getIntField(fieldName);
            return verifyFn(before, after);
        });
    }

    void assertIncrementsCumulativeMetricsField(
        ShardingDataTransformInstanceMetrics* metrics,
        const std::function<void(ShardingDataTransformInstanceMetrics*)>& mutateFn,
        Section section,
        StringData fieldName) {
        assertAltersCumulativeMetricsField(
            metrics, mutateFn, section, fieldName, [](auto before, auto after) {
                return after > before;
            });
    }

    void assertDecrementsCumulativeMetricsField(
        ShardingDataTransformInstanceMetrics* metrics,
        const std::function<void(ShardingDataTransformInstanceMetrics*)>& mutateFn,
        Section section,
        StringData fieldName) {
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
    const BSONObj kTestCommand = BSON("command"
                                      << "test");

    virtual std::unique_ptr<ShardingDataTransformCumulativeMetrics> initializeCumulativeMetrics() {
        return std::make_unique<ShardingDataTransformCumulativeMetrics>(
            kTestMetricsName,
            std::make_unique<ShardingDataTransformCumulativeMetricsFieldNameProviderForTest>());
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

    ShardingDataTransformCumulativeMetrics* getCumulativeMetrics() {
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

    std::unique_ptr<ShardingDataTransformCumulativeMetrics> _cumulativeMetrics;
    std::vector<ShardingDataTransformCumulativeMetrics::UniqueScopedObserver> _observers;
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
