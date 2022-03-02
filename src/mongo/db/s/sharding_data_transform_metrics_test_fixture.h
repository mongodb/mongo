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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/sharding_data_transform_instance_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

class ObserverMock : public ShardingDataTransformMetricsObserverInterface {
public:
    constexpr static auto kDefaultRole = ShardingDataTransformMetrics::Role::kCoordinator;
    ObserverMock(int64_t startTime, int64_t timeRemaining)
        : ObserverMock{startTime, timeRemaining, timeRemaining, kDefaultRole} {}
    ObserverMock(int64_t startTime,
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

    virtual int64_t getHighEstimateRemainingTimeMillis() const override {
        return _timeRemainingHigh;
    }

    virtual int64_t getLowEstimateRemainingTimeMillis() const override {
        return _timeRemainingLow;
    }

    virtual int64_t getStartTimestamp() const override {
        return _startTime;
    }

    virtual ShardingDataTransformMetrics::Role getRole() const override {
        return _role;
    }

private:
    UUID _uuid;
    int64_t _startTime;
    int64_t _timeRemainingHigh;
    int64_t _timeRemainingLow;
    ShardingDataTransformMetrics::Role _role;
};

class ShardingDataTransformMetricsTestFixture : public unittest::Test {
protected:
    constexpr static auto kTestMetricsName = "testMetrics";
    constexpr static int64_t kYoungestTime = std::numeric_limits<int64_t>::max();
    constexpr static int64_t kOldestTime = 1;
    using Role = ShardingDataTransformInstanceMetrics::Role;
    const NamespaceString kTestNamespace = NamespaceString("test.source");
    const BSONObj kTestCommand = BSON("command"
                                      << "test");

    ShardingDataTransformMetricsTestFixture() : _cumulativeMetrics{kTestMetricsName} {}

    const ObserverMock* getYoungestObserver() {
        static StaticImmortal<ObserverMock> youngest{kYoungestTime, kYoungestTime};
        return &youngest.value();
    }

    const ObserverMock* getOldestObserver() {
        static StaticImmortal<ObserverMock> oldest{kOldestTime, kOldestTime};
        return &oldest.value();
    }

    using SpecialIndexBehaviorMap = stdx::unordered_map<int, std::function<void()>>;
    const SpecialIndexBehaviorMap kNoSpecialBehavior{};
    SpecialIndexBehaviorMap registerAtIndex(int index, const ObserverMock* mock) {
        return SpecialIndexBehaviorMap{
            {index, [=] { auto ignore = _cumulativeMetrics.registerInstanceMetrics(mock); }}};
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
            auto time = rng.nextInt64(kYoungestTime - 1) + 1;
            inserted.emplace_back(
                std::make_unique<ScopedObserverType>(time, time, &_cumulativeMetrics));
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
        ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                      ObserverMock::kDefaultRole),
                  kOldestTime);
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
                [=, &storage, specialBehavior = std::move(specialBehavior), &done] {
                    performRandomOperations(
                        storage, kIterations, kRemovalOdds, seed, specialBehavior);
                    done.emplaceValue();
                });
        }
        for (auto& pf : threadPFs) {
            pf.future.wait();
        }
        ASSERT_EQ(_cumulativeMetrics.getOldestOperationHighEstimateRemainingTimeMillis(
                      ObserverMock::kDefaultRole),
                  kOldestTime);
        size_t expectedCount = 1;  // Special insert for kOldest is not counted in vector size.
        for (auto& v : threadStorage) {
            expectedCount += v.size();
        }
        ASSERT_EQ(_cumulativeMetrics.getObservedMetricsCount(), expectedCount);
        for (auto& t : threads) {
            t.join();
        }
    }

    ShardingDataTransformCumulativeMetrics _cumulativeMetrics;
};

}  // namespace mongo
