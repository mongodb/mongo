// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/observable_mutex_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <iterator>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    std::string_view tag;
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

    static constexpr std::string_view kInternalMutexTags[] = {
        ObservableMutexRegistry::kRegistrationMutexTag,
        ObservableMutexRegistry::kCollectionMutexTag};

    std::unique_ptr<MockObservableMutex> makeMutexAndAddToRegistry(
        TaggedStats stats, boost::optional<std::string_view> instanceLabel = boost::none) {
        auto m = std::make_unique<MockObservableMutex>(stats.data);
        _registry.add(stats.tag, m->getMutex(), instanceLabel);
        return m;
    }

    struct Options {
        bool listAll = false;
        bool skipInternalMutexes = true;
    };

    void assertReport(StatsList expected, Options options) {
        auto stats = _registry.report(options.listAll);
        _compare(options, stats, expected);
    }

    void assertStatsPerTag(const StatsList& expected) {
        auto stats = _registry.statsPerTag();

        const size_t expectedSize = expected.size() + std::size(kInternalMutexTags);
        ASSERT_EQ(stats.size(), expectedSize);

        for (const auto& [tag, data, _] : expected) {
            auto it = stats.find(tag);
            ASSERT(it != stats.end()) << "Tag not found: " << tag;
            const auto& ex = it->second.exclusiveAcquisitions;
            const auto& sh = it->second.sharedAcquisitions;

            EXPECT_EQ(ex.total, data.exclusiveAcquisitions.total) << "for tag " << tag;
            EXPECT_EQ(ex.contentions, data.exclusiveAcquisitions.contentions) << "for tag " << tag;
            EXPECT_EQ(ex.waitCycles, data.exclusiveAcquisitions.waitCycles) << "for tag " << tag;
            EXPECT_EQ(sh.total, data.sharedAcquisitions.total) << "for tag " << tag;
            EXPECT_EQ(sh.contentions, data.sharedAcquisitions.contentions) << "for tag " << tag;
            EXPECT_EQ(sh.waitCycles, data.sharedAcquisitions.waitCycles) << "for tag " << tag;
        }
    }

private:
    void _compare(Options options, BSONObj actual, StatsList expected) {
        auto assertStats =
            [](BSONObj statObj, MutexAcquisitionStats expected, std::string_view tag) {
                ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalAcquisitionsFieldName),
                          expected.total)
                    << "for tag " << tag;
                ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalContentionsFieldName),
                          expected.contentions)
                    << "for tag " << tag;
                ASSERT_EQ(statObj.getIntField(ObservableMutexRegistry::kTotalWaitCyclesFieldName),
                          expected.waitCycles)
                    << "for tag " << tag;
            };

        const std::size_t expectedSize = options.skipInternalMutexes
            ? expected.size() + std::size(kInternalMutexTags)
            : expected.size();

        ASSERT_EQ(actual.nFields(), expectedSize);
        for (const auto& [tag, data, listAllData] : expected) {
            if (options.skipInternalMutexes &&
                std::find(std::begin(kInternalMutexTags), std::end(kInternalMutexTags), tag) !=
                    std::end(kInternalMutexTags)) {
                continue;
            }

            auto tagStats = actual.getObjectField(tag);
            assertStats(tagStats.getObjectField(ObservableMutexRegistry::kExclusiveFieldName),
                        data.exclusiveAcquisitions,
                        tag);
            assertStats(tagStats.getObjectField(ObservableMutexRegistry::kSharedFieldName),
                        data.sharedAcquisitions,
                        tag);

            ASSERT_EQ(options.listAll, tagStats.hasField(ObservableMutexRegistry::kMutexFieldName));
            if (options.listAll) {
                ASSERT(listAllData);
                auto subArr = tagStats[ObservableMutexRegistry::kMutexFieldName].Array();

                for (size_t i = 0; i < subArr.size(); ++i) {
                    auto obj = subArr.at(i).Obj();
                    ASSERT_TRUE(obj.hasField(ObservableMutexRegistry::kRegisteredFieldName));
                    ASSERT_EQ(obj.getField(ObservableMutexRegistry::kIdFieldName).Long(),
                              *listAllData->at(i).mutexId + std::size(kInternalMutexTags));

                    if (const auto& instanceLabel = listAllData->at(i).instanceLabel) {
                        ASSERT_EQ(
                            obj.getStringField(ObservableMutexRegistry::kInstanceLabelFieldName),
                            *instanceLabel);
                    } else {
                        ASSERT_FALSE(
                            obj.hasField(ObservableMutexRegistry::kInstanceLabelFieldName));
                    }

                    assertStats(obj.getObjectField(ObservableMutexRegistry::kExclusiveFieldName),
                                listAllData->at(i).data.exclusiveAcquisitions,
                                tag);
                    assertStats(obj.getObjectField(ObservableMutexRegistry::kSharedFieldName),
                                listAllData->at(i).data.sharedAcquisitions,
                                tag);
                }
            }
        }
    }

    ObservableMutexRegistry _registry;
};

TEST_F(ObservableMutexRegistryReportTest, SelfRegistry) {
    // 1. Register _registrationMutex
    // 2. Register _collectionMutex
    // 3. Grab new registrations in `report()`.
    const int numRegistrationAcquisitions = 3;
    // 1. `report()` in `assertReport(...)`.
    const int numCollectionAcquisitions = 1;
    const std::vector<TaggedStats> expected = {
        {ObservableMutexRegistry::kRegistrationMutexTag,
         {{numRegistrationAcquisitions, 0, 0}, {0, 0, 0}}},
        {ObservableMutexRegistry::kCollectionMutexTag,
         {{numCollectionAcquisitions, 0, 0}, {0, 0, 0}}},
    };
    assertReport(expected, {.listAll = false, .skipInternalMutexes = false});
}

TEST_F(ObservableMutexRegistryReportTest, ExternallyEmptyRegistry) {
    assertReport({}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, ZeroStats) {
    TaggedStats statsA = {"a", {{0, 0, 0}, {0, 0, 0}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    assertReport({statsA}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, NonZeroStats) {
    TaggedStats statsA = {"a", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    assertReport({statsA}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, MultipleMutexes) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"b", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);

    // The stats for tag A should be aggregated.
    TaggedStats statsA = {"a", statsA0.data + statsA1.data};
    assertReport({statsA, statsB}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, InvalidatingMutexBeforeVisitingStillEmitsStats) {
    TaggedStats statsA = {"a", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    mutex->invalidate();
    assertReport({statsA}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, SomeMutexInvalidated) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"b", {{7, 3, 1000}, {0, 0, 0}}};
    TaggedStats statsC = {"c", {{4012, 3552, 7886549}, {1, 1, 1}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);
    auto mutexC = makeMutexAndAddToRegistry(statsC);

    TaggedStats statsA = {"a", statsA0.data + statsA1.data};
    assertReport({statsA, statsB, statsC}, {.listAll = false, .skipInternalMutexes = true});

    // Invalidating a mutex should not remove its stats from the report when listAll is disabled.
    mutexA1->invalidate();
    assertReport({statsA, statsB, statsC}, {.listAll = false, .skipInternalMutexes = true});

    mutexA0->invalidate();
    mutexB->invalidate();
    assertReport({statsA, statsB, statsC}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, ListAll) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"b", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);

    TaggedStats statsA = {"a", statsA0.data + statsA1.data};
    auto dummyTimestamp = Date_t::now();

    // These vectors represent the expected non-aggregated stats when listAll is enabled.
    std::vector<StatsRecord> listAllA = {{statsA0.data, 0 /* mutexId */, dummyTimestamp},
                                         {statsA1.data, 1 /* mutexId */, dummyTimestamp}};
    std::vector<StatsRecord> listAllB = {{statsB.data, 2 /* mutexId */, dummyTimestamp}};
    assertReport({{"a", statsA.data, listAllA}, {"b", statsB.data, listAllB}},
                 {.listAll = true, .skipInternalMutexes = true});

    // Only valid mutexes should be included in the listAll portion of the report..
    mutexB->invalidate();
    listAllB.clear();
    assertReport({{"a", statsA.data, listAllA}, {"b", statsB.data, listAllB}},
                 {.listAll = true, .skipInternalMutexes = true});

    // When listAll is disabled, we should still see stats for the invalidated mutex.
    assertReport({{statsA, statsB}}, {.listAll = false, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, ListAllWithInstanceLabel) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0, "my-pool"sv);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);

    auto dummyTimestamp = Date_t::now();

    std::vector<StatsRecord> listAllA = {
        {statsA0.data, 0, dummyTimestamp, std::string("my-pool")},
        {statsA1.data, 1, dummyTimestamp},
    };

    assertReport({{"a", statsA0.data + statsA1.data, listAllA}},
                 {.listAll = true, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, MutexIdShouldNeverChangeAfterInvalidation) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsA2 = {"a", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexA2 = makeMutexAndAddToRegistry(statsA2);

    TaggedStats statsA = {"a", statsA0.data + statsA1.data + statsA2.data};
    auto dummyTimestamp = Date_t::now();

    // This vector represents the expected non-aggregated stats when listAll is enabled.
    std::vector<StatsRecord> listAllA = {{statsA0.data, 0 /* mutexId */, dummyTimestamp},
                                         {statsA1.data, 1 /* mutexId */, dummyTimestamp},
                                         {statsA2.data, 2 /* mutexId */, dummyTimestamp}};
    assertReport({{"a", statsA.data, listAllA}}, {.listAll = true, .skipInternalMutexes = true});

    // mutexA2 should still have id 2 even after mutexA1 is invalidated.
    mutexA1->invalidate();
    listAllA.erase(listAllA.begin() + 1);
    assertReport({{"a", statsA.data, listAllA}}, {.listAll = true, .skipInternalMutexes = true});
}

TEST_F(ObservableMutexRegistryReportTest, StatsPerTagSingleMutex) {
    TaggedStats statsA = {"a", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    assertStatsPerTag({statsA});
}

TEST_F(ObservableMutexRegistryReportTest, StatsPerTagAggregatesMultipleMutexesUnderSameTag) {
    TaggedStats statsA0 = {"a", {{3, 2, 500}, {1, 0, 17}}};
    TaggedStats statsA1 = {"a", {{5, 1, 200}, {4, 2, 50}}};
    TaggedStats statsB = {"b", {{7, 3, 1000}, {0, 0, 0}}};

    auto mutexA0 = makeMutexAndAddToRegistry(statsA0);
    auto mutexA1 = makeMutexAndAddToRegistry(statsA1);
    auto mutexB = makeMutexAndAddToRegistry(statsB);

    TaggedStats statsA = {"a", statsA0.data + statsA1.data};
    assertStatsPerTag({statsA, statsB});
}

TEST_F(ObservableMutexRegistryReportTest, StatsPerTagPreservesInvalidatedMutexStats) {
    TaggedStats statsA = {"a", {{3, 2, 500}, {1, 0, 17}}};
    auto mutex = makeMutexAndAddToRegistry(statsA);

    mutex->invalidate();
    assertStatsPerTag({statsA});
}

TEST_F(ObservableMutexRegistryReportTest, RejectsInvalidOtelTag) {
    for (std::string_view tag :
         {"", "Mutex", "1mutex", "my.mutex", "my_Mutex", "my_", "my__mutex"}) {
        ASSERT_THROWS_CODE(
            makeMutexAndAddToRegistry({tag, {}}), DBException, ErrorCodes::InvalidOptions);
    }
}

}  // namespace
}  // namespace mongo
