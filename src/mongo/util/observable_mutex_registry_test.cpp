/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/util/observable_mutex_registry.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class DummyMutex {
public:
    void lock();
    void unlock();
};

using StatsRecord = ObservableMutexRegistry::StatsRecord;

/**
 * Models the expected stats returned from the ObservableMutexCollector report BSONObj. See
 * ObservableMutexRegistry::report() docstring for details on report structure.
 */
struct TaggedStats {
    StringData tag;
    MutexStats data;

    // Optional data for listAll if reporting in that mode.
    boost::optional<std::vector<StatsRecord>> listAllData;
};

class MockObservableMutex {
public:
    explicit MockObservableMutex(MutexStats stats) {
        _mutex.setExclusiveAcquisitions_forTest(stats.exclusiveAcquisitions);
        _mutex.setSharedAcquisitions_forTest(stats.sharedAcquisitions);
    }

    ObservableMutex<DummyMutex>& getMutex() {
        return _mutex;
    }

    void invalidate() {
        _mutex.token()->invalidate();
    }

private:
    ObservableMutex<DummyMutex> _mutex;
};

class ObservableMutexRegistryReportTest : public unittest::Test {
public:
    using StatsList = std::vector<TaggedStats>;

    std::unique_ptr<MockObservableMutex> makeMutexAndAddToRegistry(TaggedStats stats) {
        auto m = std::make_unique<MockObservableMutex>(stats.data);
        _registry.add(stats.tag, m->getMutex());
        return m;
    }

    void runTest(StatsList expected, bool listAll = false) {
        auto stats = _registry.report(listAll);
        _compare(listAll, stats, expected);
    }

private:
    void _compare(bool listAll, BSONObj actual, StatsList expected) {
        auto assertStats = [](BSONObj statObj, MutexAcquisitionStats expected) {
            ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalAcquisitionsFieldName),
                      expected.total);
            ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalContentionsFieldName),
                      expected.contentions);
            ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalWaitCyclesFieldName),
                      expected.waitCycles);
        };

        ASSERT_EQ(actual.nFields(), expected.size());
        for (const auto& [tag, data, listAllData] : expected) {
            auto tagStats = actual.getObjectField(tag);
            assertStats(tagStats.getObjectField(ObservableMutexRegistry::kExclusiveFieldName),
                        data.exclusiveAcquisitions);
            assertStats(tagStats.getObjectField(ObservableMutexRegistry::kSharedFieldName),
                        data.sharedAcquisitions);

            ASSERT_EQ(listAll, tagStats.hasField(ObservableMutexRegistry::kMutexFieldName));
            if (listAll) {
                ASSERT(listAllData);
                auto subArr = tagStats[ObservableMutexRegistry::kMutexFieldName].Array();

                for (size_t i = 0; i < subArr.size(); ++i) {
                    auto obj = subArr.at(i).Obj();
                    ASSERT_TRUE(obj.hasField(ObservableMutexRegistry::kRegisteredFieldName));
                    ASSERT_EQ(obj.getField(ObservableMutexRegistry::kIdFieldName).Long(),
                              *listAllData->at(i).mutexId);

                    assertStats(obj.getObjectField(ObservableMutexRegistry::kExclusiveFieldName),
                                listAllData->at(i).data.exclusiveAcquisitions);
                    assertStats(obj.getObjectField(ObservableMutexRegistry::kSharedFieldName),
                                listAllData->at(i).data.sharedAcquisitions);
                }
            }
        }
    }

    ObservableMutexRegistry _registry;
};

TEST_F(ObservableMutexRegistryReportTest, EmptyRegistry) {
    runTest({});
}

TEST_F(ObservableMutexRegistryReportTest, ZeroStats) {
    TaggedStats statsA = {"A", {{0, 0, 0}, {0, 0, 0}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    runTest({statsA});
}

TEST_F(ObservableMutexRegistryReportTest, NonZeroStats) {
    TaggedStats statsA = {"A", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    runTest({statsA});
}

TEST_F(ObservableMutexRegistryReportTest, MultipleMutexes) {
    TaggedStats statsA0 = {"A", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"A", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"B", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);

    // The stats for tag A should be aggregated.
    TaggedStats statsA = {"A", statsA0.data + statsA1.data};
    runTest({statsA, statsB});
}

TEST_F(ObservableMutexRegistryReportTest, InvalidatingMutexBeforeVisitingStillEmitsStats) {
    TaggedStats statsA = {"A", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    mutex->invalidate();
    runTest({statsA});
}

TEST_F(ObservableMutexRegistryReportTest, SomeMutexInvalidated) {
    TaggedStats statsA0 = {"A", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"A", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"B", {{7, 3, 1000}, {0, 0, 0}}};
    TaggedStats statsC = {"C", {{4012, 3552, 7886549}, {1, 1, 1}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);
    auto mutexC = makeMutexAndAddToRegistry(statsC);

    TaggedStats statsA = {"A", statsA0.data + statsA1.data};
    runTest({statsA, statsB, statsC});

    // Invalidating a mutex should not remove its stats from the report when listAll is disabled.
    mutexA1->invalidate();
    runTest({statsA, statsB, statsC});

    mutexA0->invalidate();
    mutexB->invalidate();
    runTest({statsA, statsB, statsC});
}

TEST_F(ObservableMutexRegistryReportTest, ListAll) {
    TaggedStats statsA0 = {"A", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"A", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"B", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);

    TaggedStats statsA = {"A", statsA0.data + statsA1.data};
    auto dummyTimestamp = Date_t::now();

    // These vectors represent the expected non-aggregated stats when listAll is enabled.
    std::vector<StatsRecord> listAllA = {{statsA0.data, 0 /* mutexId */, dummyTimestamp},
                                         {statsA1.data, 1 /* mutexId */, dummyTimestamp}};
    std::vector<StatsRecord> listAllB = {{statsB.data, 2 /* mutexId */, dummyTimestamp}};
    runTest({{"A", statsA.data, listAllA}, {"B", statsB.data, listAllB}}, true /* listAll */);

    // Only valid mutexes should be included in the listAll portion of the report..
    mutexB->invalidate();
    listAllB.clear();
    runTest({{"A", statsA.data, listAllA}, {"B", statsB.data, listAllB}}, true /* listAll */);

    // When listAll is disabled, we should still see stats for the invalidated mutex.
    runTest({{statsA, statsB}}, false /* listAll */);
}

TEST_F(ObservableMutexRegistryReportTest, MutexIdShouldNeverChangeAfterInvalidation) {
    TaggedStats statsA0 = {"A", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"A", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsA2 = {"A", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexA2 = makeMutexAndAddToRegistry(statsA2);

    TaggedStats statsA = {"A", statsA0.data + statsA1.data + statsA2.data};
    auto dummyTimestamp = Date_t::now();

    // This vector represents the expected non-aggregated stats when listAll is enabled.
    std::vector<StatsRecord> listAllA = {{statsA0.data, 0 /* mutexId */, dummyTimestamp},
                                         {statsA1.data, 1 /* mutexId */, dummyTimestamp},
                                         {statsA2.data, 2 /* mutexId */, dummyTimestamp}};
    runTest({{"A", statsA.data, listAllA}}, true /* listAll */);

    // mutexA2 should still have id 2 even after mutexA1 is invalidated.
    mutexA1->invalidate();
    listAllA.erase(listAllA.begin() + 1);
    runTest({{"A", statsA.data, listAllA}}, true /* listAll */);
}

}  // namespace
}  // namespace mongo
