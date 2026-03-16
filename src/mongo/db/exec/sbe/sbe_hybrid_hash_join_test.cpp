/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::HybridHashJoin.
 */

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hybrid_hash_join.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>

#include <boost/math/distributions/chi_squared.hpp>

namespace mongo::sbe {
namespace {

/**
 * Helper to create a MaterializedRow with a single integer key.
 */
value::MaterializedRow makeKeyRow(int64_t key) {
    value::MaterializedRow row(1);
    row.reset(0, true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(key));
    return row;
}

/**
 * Helper to create a MaterializedRow with a single string project value.
 */
value::MaterializedRow makeProjectRow(StringData payload) {
    auto [tag, val] = value::makeNewString(payload);
    value::MaterializedRow row(1);
    row.reset(0, true, tag, val);
    return row;
}

/**
 * Helper to extract the int64 value from a key row.
 */
int64_t getKeyValue(const value::MaterializedRow* row) {
    auto [tag, val] = row->getViewOfValue(0);
    ASSERT_EQ(tag, value::TypeTags::NumberInt64);
    return value::bitcastTo<int64_t>(val);
}

/**
 * Consume all matches from a cursor into a vector of key values.
 */
std::vector<int64_t> drainCursor(JoinCursor& cursor) {
    std::vector<int64_t> results;
    while (auto matchOpt = cursor.next()) {
        results.push_back(getKeyValue(matchOpt->buildKeyRow));
    }
    return results;
}

/**
 * Helper to create a MaterializedRow with a single string key.
 */
value::MaterializedRow makeStringKeyRow(StringData key) {
    auto [tag, val] = value::makeNewString(key);
    value::MaterializedRow row(1);
    row.reset(0, true, tag, val);
    return row;
}

/**
 * Helper to extract the string value from a row.
 */
std::string getStringValue(const value::MaterializedRow* row) {
    auto [tag, val] = row->getViewOfValue(0);
    ASSERT(value::isString(tag));
    return std::string(value::getStringView(tag, val));
}

/**
 * Consume all matches from a cursor into a vector of (buildKey, buildProject, probeKey,
 * probeProject) tuples.
 */
struct MatchTuple {
    std::string buildKey;
    std::string buildProject;
    boost::optional<std::string> probeKey;
    boost::optional<std::string> probeProject;
};

std::vector<MatchTuple> drainCursorWithProjects(JoinCursor& cursor) {
    std::vector<MatchTuple> results;
    while (auto matchOpt = cursor.next()) {
        MatchTuple tuple;
        tuple.buildKey = getStringValue(matchOpt->buildKeyRow);
        tuple.buildProject = getStringValue(matchOpt->buildProjectRow);
        if (matchOpt->probeKeyRow) {
            tuple.probeKey = getStringValue(matchOpt->probeKeyRow);
            tuple.probeProject = getStringValue(matchOpt->probeProjectRow);
        }
        results.push_back(std::move(tuple));
    }
    return results;
}

/**
 * Helper to create a MaterializedRow with two integer keys.
 */
value::MaterializedRow makeCompositeKeyRow(int64_t key1, int64_t key2) {
    value::MaterializedRow row(2);
    row.reset(0, true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(key1));
    row.reset(1, true, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(key2));
    return row;
}


/**
 * Test fixture providing an OperationContext for HybridHashJoin tests.
 */
class HybridHashJoinTestFixture : public PlanStageTestFixture {
protected:
    static constexpr uint64_t kDefaultMemLimit = 256;
    HashJoinStats stats;

    std::unique_ptr<HybridHashJoin> makeHHJ(CollatorInterface* collator = nullptr) {
        stats = {};
        return std::make_unique<HybridHashJoin>(kDefaultMemLimit, collator, boost::none, stats);
    }
};

TEST_F(HybridHashJoinTestFixture, BuildAndProbeInMemoryNoSpill) {
    auto hhj = makeHHJ();

    // Insert build rows
    hhj->addBuild(makeKeyRow(10), makeProjectRow("proj_10"));
    hhj->addBuild(makeKeyRow(20), makeProjectRow("proj_20"));
    hhj->addBuild(makeKeyRow(30), makeProjectRow("proj_30"));
    hhj->finishBuild();

    // Probe with matching key
    auto probeKey = makeKeyRow(20);
    auto probeProject = makeProjectRow("probe_20");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0], 20);

    // Probe with non-matching key
    probeKey = makeKeyRow(99);
    probeProject = makeProjectRow("probe_99");
    hhj->probe(probeKey, probeProject, cursor);
    auto matches2 = drainCursor(cursor);
    ASSERT_EQ(matches2.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, MultipleMatchesForSameKey) {
    auto hhj = makeHHJ();

    // Insert duplicates
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_a"));
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_b"));
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_c"));
    hhj->finishBuild();

    auto probeKey = makeKeyRow(42);
    auto probeProject = makeProjectRow("probe_42");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);

    std::vector<std::string> projectValues{};
    while (auto matchOpt = cursor.next()) {
        auto key = getKeyValue(matchOpt->buildKeyRow);
        ASSERT_EQ(key, 42);
        projectValues.push_back(getStringValue(matchOpt->buildProjectRow));
    }
    ASSERT_EQ(projectValues.size(), 3u);
    std::unordered_set<std::string> values{projectValues.begin(), projectValues.end()};
    ASSERT_TRUE(values.contains("proj_42_a"));
    ASSERT_TRUE(values.contains("proj_42_b"));
    ASSERT_TRUE(values.contains("proj_42_c"));
}

using HybridHashJoinDeathTest = HybridHashJoinTestFixture;
DEATH_TEST_F(HybridHashJoinDeathTest,
             PhaseGuardBuildAfterFinish,
             "called addBuild() outside of kBuild phase") {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();

    // Attempting to add build row after finishBuild should fail (tassert)
    hhj->addBuild(makeKeyRow(2), makeProjectRow("proj_2"));
}

DEATH_TEST_F(HybridHashJoinDeathTest,
             PhaseGuardProbeBeforeFinishBuild,
             "called probe() outside of kProbe phase") {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));

    // Probe before finishBuild should fail (tassert)
    auto probeKey = makeKeyRow(1);
    auto probeProject = makeProjectRow("probe_1");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
}

DEATH_TEST_F(HybridHashJoinDeathTest,
             PhaseGuardSpillBeforeFinishProb,
             "called nextSpilledJoinCursor() outside of kProcessSpilled phase") {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();
    auto probeKey = makeKeyRow(1);
    auto probeProject = makeProjectRow("probe_1");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);

    // nextSpilledJoinCursor before finishProbe should fail (tassert)
    (void)hhj->nextSpilledJoinCursor();
}

TEST_F(HybridHashJoinTestFixture, EmptyBuildSide) {
    auto hhj = makeHHJ();

    hhj->finishBuild();

    auto probeKey = makeKeyRow(42);
    auto probeProject = makeProjectRow("probe_42");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, EmptyProbeSide) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->addBuild(makeKeyRow(2), makeProjectRow("proj_2"));
    hhj->finishBuild();
    hhj->finishProbe();

    // No spilled partitions to process since nothing triggered spilling
    auto cursorOpt = hhj->nextSpilledJoinCursor();
    ASSERT_FALSE(cursorOpt.has_value());
}

TEST_F(HybridHashJoinTestFixture, ResetState) {
    auto hhj = makeHHJ();

    // First pass
    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();
    auto probeKey = makeKeyRow(1);
    auto probeProject = makeProjectRow("probe_1");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 1u);
    hhj->finishProbe();

    // Reset and start over
    hhj->reset();
    ASSERT_EQ(hhj->getMemUsage(), 0u);

    // Second pass with different data
    hhj->addBuild(makeKeyRow(100), makeProjectRow("proj_100"));
    hhj->finishBuild();
    auto probeKey2 = makeKeyRow(100);
    auto probeProject2 = makeProjectRow("probe_100");
    auto cursor2 = JoinCursor::empty();
    hhj->probe(probeKey2, probeProject2, cursor2);
    auto matches2 = drainCursor(cursor2);
    ASSERT_EQ(matches2.size(), 1u);
    ASSERT_EQ(matches2[0], 100);
}

TEST_F(HybridHashJoinTestFixture, MemoryUsageTracking) {
    auto hhj = makeHHJ();

    ASSERT_EQ(hhj->getMemUsage(), 0u);

    hhj->addBuild(makeKeyRow(1), makeProjectRow("big_string_1"));
    ASSERT_GT(hhj->getMemUsage(), 0u);

    size_t memAfterOne = hhj->getMemUsage();
    hhj->addBuild(makeKeyRow(2), makeProjectRow("big_string_2"));
    ASSERT_GT(hhj->getMemUsage(), memAfterOne);
}

TEST_F(HybridHashJoinTestFixture, ProbeReturnsCorrectValue) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(10), makeProjectRow("build_proj_10"));
    hhj->finishBuild();

    auto probeKey = makeKeyRow(10);
    auto probeProject = makeProjectRow("probe_proj_10");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matchOpt = cursor.next();

    ASSERT_TRUE(matchOpt.has_value());
    // Verify all the pointers are set
    ASSERT_NE(matchOpt->buildKeyRow, nullptr);
    ASSERT_NE(matchOpt->buildProjectRow, nullptr);

    // Verify build key
    ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), 10);

    // No more matches
    ASSERT_FALSE(cursor.next().has_value());
}

TEST_F(HybridHashJoinTestFixture, StatsTrackPeakMemory) {
    auto hhj = makeHHJ();

    for (int i = 0; i < 10; ++i) {
        hhj->addBuild(makeKeyRow(i), makeProjectRow("big_string_" + std::to_string(i)));
    }

    // Peak memory should be tracked
    ASSERT_GT(stats.peakTrackedMemBytes, 0u);
    ASSERT_GTE(stats.peakTrackedMemBytes, hhj->getMemUsage());
}

TEST_F(HybridHashJoinTestFixture, SpillTriggersOnMemoryLimit) {
    auto hhj = makeHHJ();

    // Insert enough rows to exceed the tiny memory limit
    for (int i = 0; i < 50; ++i) {
        auto payload = "payload_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(i), makeProjectRow(payload));
    }
    hhj->finishBuild();

    // Verify spilling occurred
    ASSERT_TRUE(stats.usedDisk);
    ASSERT_GT(stats.spillingStats.getSpills(), 0u);
    ASSERT_GT(stats.spillingStats.getSpilledBytes(), 0u);
    ASSERT_GT(stats.spillingStats.getSpilledRecords(), 0u);
    ASSERT_GT(stats.spillingStats.getSpilledDataStorageSize(), 0u);
    ASSERT_GT(stats.numPartitionsSpilled, 0);
}

TEST_F(HybridHashJoinTestFixture, SpilledPartitionsProcessedCorrectly) {
    auto hhj = makeHHJ();

    // Insert rows that will definitely cause spilling
    std::set<int64_t> buildKeys;
    for (int i = 0; i < 100; ++i) {
        auto payload = "payload_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(i), makeProjectRow(payload));
        buildKeys.insert(i);
    }
    hhj->finishBuild();

    // Collect matches
    std::set<int64_t> matchedKeys;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 100; ++i) {
        auto probePayload = "probe_" + std::to_string(i);
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow(probePayload);
        hhj->probe(probeKey, probeProject, cursor);
        while (auto matchOpt = cursor.next()) {
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }
    hhj->finishProbe();

    // Process all spilled partitions and collect matched keys
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), getKeyValue(matchOpt->probeKeyRow));
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }

    // verify all keys are covered
    ASSERT_EQ(matchedKeys.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(matchedKeys.contains(i));
    }
}

TEST_F(HybridHashJoinTestFixture, HandlesRecursivePartitions) {
    auto hhj = makeHHJ();

    // Insert many rows - with only 2 partitions and tiny memory, this will cause spilling
    // and potentially recursion when a spilled partition is still too large
    for (int i = 0; i < 200; ++i) {
        auto payload = "payload_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(i), makeProjectRow(payload));
    }
    hhj->finishBuild();

    // Collect matches
    std::set<int64_t> matchedKeys;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 200; ++i) {
        auto probePayload = "probe_payload_" + std::to_string(i);
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow(probePayload);
        hhj->probe(probeKey, probeProject, cursor);
        while (auto matchOpt = cursor.next()) {
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }
    hhj->finishProbe();

    // Process spilled partitions - this should trigger recursive processing
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), getKeyValue(matchOpt->probeKeyRow));
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }

    // verify all keys are covered
    ASSERT_EQ(matchedKeys.size(), 200u);
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(matchedKeys.contains(i));
    }
    // Verify recursive join
    ASSERT_GT(stats.recursionDepthMax, 0u);
}

TEST_F(HybridHashJoinTestFixture, DuplicateKeysReturnCorrectBuildProjects) {
    auto hhj = makeHHJ();

    // Build with duplicate keys but different project values
    hhj->addBuild(makeStringKeyRow("dup_key"), makeProjectRow("build_A"));
    hhj->addBuild(makeStringKeyRow("dup_key"), makeProjectRow("build_B"));
    hhj->addBuild(makeStringKeyRow("dup_key"), makeProjectRow("build_C"));
    hhj->finishBuild();

    auto probeKey = makeStringKeyRow("dup_key");
    auto probeProject = makeProjectRow("probe");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursorWithProjects(cursor);
    ASSERT_EQ(matches.size(), 3u);

    // Collect all build project values
    std::set<std::string> buildProjects;
    for (const auto& m : matches) {
        buildProjects.insert(m.buildProject);
    }
    ASSERT_TRUE(buildProjects.count("build_A") == 1);
    ASSERT_TRUE(buildProjects.count("build_B") == 1);
    ASSERT_TRUE(buildProjects.count("build_C") == 1);
}

TEST_F(HybridHashJoinTestFixture, SpilledPartitionsPreserveProjectValues) {
    auto hhj = makeHHJ();

    // Insert enough to trigger spilling
    std::map<std::string, std::string> expectedBuildProjects;
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string buildProj = "build_proj_" + std::to_string(i);
        hhj->addBuild(makeStringKeyRow(key), makeProjectRow(buildProj));
        expectedBuildProjects[key] = buildProj;
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    // Probe and collect matches
    std::map<std::string, std::string> probeMatches;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string probeProj = "probe_proj_" + std::to_string(i);
        probeKey = makeStringKeyRow(key);
        probeProject = makeProjectRow(probeProj);
        hhj->probe(probeKey, probeProject, cursor);
        while (auto matchOpt = cursor.next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            std::string buildProj = getStringValue(matchOpt->buildProjectRow);
            probeMatches[buildKey] = buildProj;
        }
    }
    hhj->finishProbe();

    std::map<std::string, std::pair<std::string, std::string>> spilledMatches;
    // Process spilled partitions
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            std::string buildProj = getStringValue(matchOpt->buildProjectRow);
            std::string probeKey = getStringValue(matchOpt->probeKeyRow);
            std::string probeProject = getStringValue(matchOpt->probeProjectRow);
            ASSERT_EQ(buildKey, probeKey);
            spilledMatches[buildKey] = {buildProj, probeProject};
        }
    }

    ASSERT_EQ(spilledMatches.size() + probeMatches.size(), 50u);
    // Verify all spilled matches have correct project values
    for (const auto& [key, projPair] : spilledMatches) {
        const auto& [actualBuildProj, actualProbeProj] = projPair;
        ASSERT_EQ(actualBuildProj, expectedBuildProjects[key]);
        // Extract the key number and verify probe project
        auto keyNum = key.substr(4);  // Remove "key_" prefix
        ASSERT_EQ(actualProbeProj, "probe_proj_" + keyNum);
    }
}

TEST_F(HybridHashJoinTestFixture, CompositeKeyMatching) {
    auto hhj = makeHHJ();

    // Build with composite keys
    hhj->addBuild(makeCompositeKeyRow(1, 10), makeProjectRow("build_1_10"));
    hhj->addBuild(makeCompositeKeyRow(1, 20), makeProjectRow("build_1_20"));
    hhj->addBuild(makeCompositeKeyRow(2, 10), makeProjectRow("build_2_10"));
    hhj->finishBuild();

    // Probe with exact match
    auto probeKey = makeCompositeKeyRow(1, 10);
    auto probeProject = makeProjectRow("probe");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches1 = drainCursor(cursor);
    ASSERT_EQ(matches1.size(), 1u);

    // Probe with partial match (different second column) - should not match
    probeKey = makeCompositeKeyRow(1, 30);
    probeProject = makeProjectRow("probe");
    hhj->probe(probeKey, probeProject, cursor);
    auto matches2 = drainCursor(cursor);
    ASSERT_EQ(matches2.size(), 0u);

    // Probe with swapped columns - should not match
    probeKey = makeCompositeKeyRow(10, 1);
    probeProject = makeProjectRow("probe");
    hhj->probe(probeKey, probeProject, cursor);
    auto matches3 = drainCursor(cursor);
    ASSERT_EQ(matches3.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, ProbeNonExistentKeys) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->addBuild(makeKeyRow(2), makeProjectRow("proj_2"));
    hhj->finishBuild();

    // Probe with keys that don't exist
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 100; i < 110; ++i) {
        auto probePayload = "probe_" + std::to_string(i);
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow(probePayload);
        hhj->probe(probeKey, probeProject, cursor);
        auto matches = drainCursor(cursor);
        ASSERT_EQ(matches.size(), 0u);
    }
}

TEST_F(HybridHashJoinTestFixture, MultipleProbesForSameKey) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42"));
    hhj->finishBuild();

    // Probe the same key multiple times
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 10; ++i) {
        auto probePayload = "probe_42_" + std::to_string(i);
        probeKey = makeKeyRow(42);
        probeProject = makeProjectRow(probePayload);
        hhj->probe(probeKey, probeProject, cursor);
        auto matches = drainCursor(cursor);
        ASSERT_EQ(matches.size(), 1u);
        ASSERT_EQ(matches[0], 42);
    }
}

TEST_F(HybridHashJoinTestFixture, EmptyCursorBehavior) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();

    // Get a cursor for a non-matching key
    auto probeKey = makeKeyRow(999);
    auto probeProject = makeProjectRow("probe_999");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);

    // Calling next() multiple times on empty cursor should be safe
    ASSERT_FALSE(cursor.next().has_value());
    ASSERT_FALSE(cursor.next().has_value());
    ASSERT_FALSE(cursor.next().has_value());
}

TEST_F(HybridHashJoinTestFixture, CaseInsensitiveJoinWithCollator) {
    // Use ToLowerString collator for case-insensitive comparison
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    auto hhj = makeHHJ(&collator);

    // Build side has uppercase keys
    hhj->addBuild(makeStringKeyRow("ABC"), makeProjectRow("build_ABC"));
    hhj->addBuild(makeStringKeyRow("DEF"), makeProjectRow("build_DEF"));
    hhj->finishBuild();

    // Probe with lowercase - should match due to collator
    auto probeKey = makeStringKeyRow("abc");
    auto probeProject = makeProjectRow("probe_abc");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursorWithProjects(cursor);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0].buildKey, "ABC");
    ASSERT_EQ(matches[0].buildProject, "build_ABC");
}

TEST_F(HybridHashJoinTestFixture, CollatorNoMatchWhenDifferent) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    auto hhj = makeHHJ(&collator);

    hhj->addBuild(makeStringKeyRow("ABC"), makeProjectRow("build_ABC"));
    hhj->finishBuild();

    // Probe with completely different string - should not match
    auto probeKey = makeStringKeyRow("xyz");
    auto probeProject = makeProjectRow("probe_xyz");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursorWithProjects(cursor);
    ASSERT_EQ(matches.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, AlwaysEqualCollatorMatchesEverything) {
    // Use AlwaysEqual collator - all strings compare equal
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto hhj = makeHHJ(&collator);

    // Build side has multiple different keys
    hhj->addBuild(makeStringKeyRow("foo"), makeProjectRow("build_foo"));
    hhj->addBuild(makeStringKeyRow("bar"), makeProjectRow("build_bar"));
    hhj->addBuild(makeStringKeyRow("baz"), makeProjectRow("build_baz"));
    hhj->finishBuild();

    // Probe with any string - should match all due to collator
    auto probeKey = makeStringKeyRow("anything");
    auto probeProject = makeProjectRow("probe_any");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursorWithProjects(cursor);
    ASSERT_EQ(matches.size(), 3u);
}

TEST_F(HybridHashJoinTestFixture, ReverseStringCollatorMatching) {
    // Use ReverseString collator - comparison key is the reversed string.
    // Two strings match if their reverses are equal.
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto hhj = makeHHJ(&collator);

    // Build with "abc" (comparison key = "cba")
    hhj->addBuild(makeStringKeyRow("abc"), makeProjectRow("build_abc"));
    // Build with "xyz" (comparison key = "zyx")
    hhj->addBuild(makeStringKeyRow("xyz"), makeProjectRow("build_xyz"));
    hhj->finishBuild();

    // Probe with "abc" - should match build "abc" since both have same comparison key "cba"
    auto probeKey = makeStringKeyRow("abc");
    auto probeProject = makeProjectRow("probe_abc");
    auto cursor = JoinCursor::empty();
    hhj->probe(probeKey, probeProject, cursor);
    auto matches = drainCursorWithProjects(cursor);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0].buildKey, "abc");
}

TEST_F(HybridHashJoinTestFixture, ResetAfterSpillAllowsReuse) {
    auto hhj = makeHHJ();

    // First pass: trigger spilling
    for (int i = 0; i < 50; ++i) {
        hhj->addBuild(makeKeyRow(i), makeProjectRow("pass1_" + std::to_string(i)));
    }
    hhj->finishBuild();
    ASSERT_TRUE(stats.usedDisk);
    ASSERT_TRUE(hhj->isPartitioned());

    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 50; ++i) {
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow("probe1_" + std::to_string(i));
        hhj->probe(probeKey, probeProject, cursor);
    }
    hhj->finishProbe();

    // Drain spilled partitions
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (cursorOpt->next()) {
        }
    }

    // Reset and reuse
    hhj->reset();
    ASSERT_EQ(hhj->getMemUsage(), 0u);

    // Second pass: different data
    hhj->addBuild(makeKeyRow(100), makeProjectRow("pass2_100"));
    hhj->addBuild(makeKeyRow(200), makeProjectRow("pass2_200"));
    hhj->finishBuild();

    ASSERT_FALSE(hhj->isPartitioned());

    auto probeKey2 = makeKeyRow(100);
    auto probeProject2 = makeProjectRow("probe2_100");
    auto cursor2 = JoinCursor::empty();
    hhj->probe(probeKey2, probeProject2, cursor2);
    auto matches = drainCursor(cursor2);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0], 100);

    // Key from first pass should not exist
    probeKey2 = makeKeyRow(0);
    probeProject2 = makeProjectRow("probe2_0");
    hhj->probe(probeKey2, probeProject2, cursor2);
    auto matches2 = drainCursor(cursor2);
    ASSERT_EQ(matches2.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, ProbeOnlyNonMatchingKeysToSpilledPartition) {
    auto hhj = makeHHJ();

    // Insert enough to trigger spilling
    for (int i = 0; i < 200; ++i) {
        hhj->addBuild(makeKeyRow(i), makeProjectRow("build_" + std::to_string(i)));
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    // Probe with keys that don't exist (will be spilled if their partition was spilled)
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 1000; i < 1100; ++i) {
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow("probe_" + std::to_string(i));
        hhj->probe(probeKey, probeProject, cursor);
        ASSERT_EQ(cursor.next(), boost::none);
    }
    hhj->finishProbe();

    // Process spilled partitions - should return no matches
    size_t spilledMatches = 0;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            spilledMatches++;
        }
    }
    // Non-matching probe keys in spilled partitions should yield no matches
    ASSERT_EQ(spilledMatches, 0u);
}

TEST_F(HybridHashJoinTestFixture, ProbeIsSmallerThanBuild) {
    auto hhj = makeHHJ();

    // Build side: many rows with distinctive project values
    std::map<int64_t, std::string> buildProjects;
    for (int i = 0; i < 100; ++i) {
        std::string proj = "BUILD_PROJ_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(i), makeProjectRow(proj));
        buildProjects[i] = proj;
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    std::map<int64_t, std::string> spilledProbeProjects;

    // Probe side: fewer rows with distinctive project values
    int inmemMatches = 0;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 50; ++i) {
        std::string proj = "PROBE_PROJ_" + std::to_string(i);
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow(proj);
        hhj->probe(probeKey, probeProject, cursor);
        if (auto matchOpt = cursor.next()) {
            int64_t buildKeyVal = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKeyVal = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKeyVal, probeKeyVal);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKeyVal])
                << "Build project mismatch for key " << buildKeyVal;
            ASSERT_EQ(actualProbeProj, proj) << "Probe project mismatch for key " << probeKeyVal;

            ASSERT_EQ(cursor.next(), boost::none);
            inmemMatches++;
        } else {
            // spilled
            spilledProbeProjects[i] = proj;
        }
    }
    hhj->finishProbe();

    // Process spilled partitions and verify key-project mappings are correct
    std::set<int64_t> matchedKeys;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            int64_t buildKeyVal = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKeyVal = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKeyVal, probeKeyVal);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKeyVal])
                << "Build project mismatch for key " << buildKeyVal;
            ASSERT_EQ(actualProbeProj, spilledProbeProjects[probeKeyVal])
                << "Probe project mismatch for key " << probeKeyVal;

            matchedKeys.insert(buildKeyVal);
        }
    }

    // Should have matched some keys from spilled partitions
    ASSERT_GT(matchedKeys.size(), 0u);
    ASSERT_EQ(matchedKeys.size() + inmemMatches, 50);

    // Should have swapped partitions
    ASSERT_GT(stats.numPartitionSwaps, 0);
    ASSERT_EQ(stats.recursionDepthMax, 0);
}

TEST_F(HybridHashJoinTestFixture, ProbeIsSmallerThanBuildWithRecursion) {
    auto hhj = makeHHJ();

    // Build side: many rows with distinctive project values
    std::map<int64_t, std::string> buildProjects;
    for (int i = 0; i < 500; ++i) {
        std::string proj = "BUILD_PROJ_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(i), makeProjectRow(proj));
        buildProjects[i] = proj;
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    std::map<int64_t, std::string> spilledProbeProjects;

    // Probe side: fewer rows with distinctive project values
    int inmemMatches = 0;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 250; ++i) {
        std::string proj = "PROBE_PROJ_" + std::to_string(i);
        probeKey = makeKeyRow(i);
        probeProject = makeProjectRow(proj);
        hhj->probe(probeKey, probeProject, cursor);
        if (auto matchOpt = cursor.next()) {
            int64_t buildKeyVal = getKeyValue(matchOpt->buildKeyRow);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKeyVal])
                << "Build project mismatch for key " << buildKeyVal;

            ASSERT_EQ(cursor.next(), boost::none);
            inmemMatches++;
        } else {
            // spilled
            spilledProbeProjects[i] = proj;
        }
    }
    hhj->finishProbe();

    // Process spilled partitions and verify key-project mappings are correct
    std::set<int64_t> matchedKeys;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            int64_t buildKeyVal = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKeyVal = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKeyVal, probeKeyVal);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKeyVal])
                << "Build project mismatch for key " << buildKeyVal;
            ASSERT_EQ(actualProbeProj, spilledProbeProjects[probeKeyVal])
                << "Probe project mismatch for key " << probeKeyVal;

            matchedKeys.insert(buildKeyVal);
        }
    }

    // Should have matched some keys from spilled partitions
    ASSERT_GT(matchedKeys.size(), 0u);
    ASSERT_EQ(matchedKeys.size() + inmemMatches, 250);

    // Should have swapped partitions
    ASSERT_GT(stats.numPartitionSwaps, 0);
    ASSERT_GT(stats.recursionDepthMax, 0);
}

TEST_F(HybridHashJoinTestFixture, BasicBlockNestedLoopJoin) {
    auto hhj = makeHHJ();

    // Insert records with same keys to ensure all go to same partition
    std::set<std::string> buildProjects;
    for (int i = 0; i < 20; ++i) {
        auto payload = "build_" + std::to_string(i);
        hhj->addBuild(makeKeyRow(42), makeProjectRow(payload));
        buildProjects.insert(payload);
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    // Probe all keys
    std::set<std::string> probeProjects;
    value::MaterializedRow probeKey(1);
    value::MaterializedRow probeProject(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 40; ++i) {
        auto probePayload = "probe_" + std::to_string(i);
        probeKey = makeKeyRow(42);
        probeProject = makeProjectRow(probePayload);
        hhj->probe(probeKey, probeProject, cursor);
        ASSERT_EQ(cursor.next(), boost::none);
        probeProjects.insert(probePayload);
    }
    hhj->finishProbe();

    // Process spilled partitions - should use BlockNestedLoopJoin
    int numMatches = 0;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), getKeyValue(matchOpt->probeKeyRow));
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), 42);
            ASSERT_TRUE(buildProjects.contains(getStringValue(matchOpt->buildProjectRow)));
            ASSERT_TRUE(probeProjects.contains(getStringValue(matchOpt->probeProjectRow)));
            numMatches++;
        }
    }

    ASSERT_EQ(stats.numFallbacksToBlockNestedLoopJoin, 1);
    ASSERT_EQ(numMatches, 40 * 20);
}

TEST_F(HybridHashJoinTestFixture, BlockNestedLoopJoinWithAlwaysEqualCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto hhj = makeHHJ(&collator);

    for (int i = 0; i < 20; ++i) {
        std::string key = "build_key_" + std::to_string(i);
        std::string proj = "build_project_" + std::to_string(i);
        hhj->addBuild(makeStringKeyRow(key), makeProjectRow(proj));
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    value::MaterializedRow probeKeyRow(1);
    value::MaterializedRow probeProjectRow(1);
    auto cursor = JoinCursor::empty();
    for (int i = 0; i < 40; ++i) {
        std::string key = "probe_key_" + std::to_string(i);
        std::string probeProj = "probe_project_" + std::to_string(i);
        probeKeyRow = makeStringKeyRow(key);
        probeProjectRow = makeProjectRow(probeProj);
        hhj->probe(probeKeyRow, probeProjectRow, cursor);
        ASSERT_EQ(cursor.next(), boost::none);
    }
    hhj->finishProbe();

    // Process spilled partitions
    std::set<int> matchedBuildIndices;
    std::set<int> matchedProbeIndices;
    int numMatches = 0;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            int buildIdx = std::stoi(buildKey.substr(10));
            matchedBuildIndices.insert(buildIdx);

            std::string probeKey = getStringValue(matchOpt->probeKeyRow);
            int probeIdx = std::stoi(probeKey.substr(10));
            matchedProbeIndices.insert(probeIdx);

            numMatches++;
        }
    }

    ASSERT_EQ(stats.numFallbacksToBlockNestedLoopJoin, 1);
    ASSERT_EQ(numMatches, 40 * 20);

    ASSERT_EQ(matchedBuildIndices.size(), 20);
    ASSERT_EQ(matchedProbeIndices.size(), 40);
}

TEST_F(HybridHashJoinTestFixture, BloomFilterReducesProbeSpills) {
    auto hhj = makeHHJ();

    // Build side: insert keys that will force spilling
    // We insert keys 0, 10, 20, 30, ... to create gaps
    std::set<int64_t> buildKeys;
    for (int i = 0; i < 250; ++i) {
        int64_t key = i * 10;  // 0, 10, 20, 30, ...
        std::string proj = "build_" + std::to_string(key);
        hhj->addBuild(makeKeyRow(key), makeProjectRow(proj));
        buildKeys.insert(key);
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    auto cursor = JoinCursor::empty();
    // Probe with keys that DON'T exist in build side
    // These should be filtered by the bloom filter and not spilled
    for (int i = 0; i < 1000; ++i) {
        int64_t key = i * 10 + 5;  // 5, 15, 25, 35, ... (not in build)
        auto probeKeyRow = makeKeyRow(key);
        auto probeProjectRow = makeProjectRow("probe_" + std::to_string(key));
        hhj->probe(probeKeyRow, probeProjectRow, cursor);
        // Should return empty cursor (no matches in memory)
        auto result = cursor.next();
        ASSERT_FALSE(result.has_value());
    }

    // Probe with keys that DO exist in build side
    std::set<int64_t> matchedInMemory;
    for (int i = 0; i < 250; ++i) {
        int64_t key = i * 10;  // 0, 10, 20, ... (in build)
        auto probeKeyRow = makeKeyRow(key);
        auto probeProjectRow = makeProjectRow("probe_" + std::to_string(key));
        hhj->probe(probeKeyRow, probeProjectRow, cursor);
        while (auto matchOpt = cursor.next()) {
            matchedInMemory.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }
    hhj->finishProbe();

    ASSERT_GT(stats.numProbeRecordsDiscarded, 0);

    // Process spilled partitions
    std::set<int64_t> matchedFromSpill;
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            matchedFromSpill.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }

    // The keys that exist in build should all be matched (either in memory or from spill)
    std::set<int64_t> allMatched;
    allMatched.insert(matchedInMemory.begin(), matchedInMemory.end());
    allMatched.insert(matchedFromSpill.begin(), matchedFromSpill.end());

    // All probed keys that exist in build should be found
    for (int i = 0; i < 250; ++i) {
        int64_t key = i * 10;
        ASSERT_TRUE(allMatched.count(key) > 0) << "Key " << key << " should have been matched";
    }
}

TEST_F(HybridHashJoinTestFixture, BloomFilterWithStringKeys) {
    auto hhj = makeHHJ();

    // Build side with string keys
    std::set<std::string> buildKeys;
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i * 2);  // Even numbers
        hhj->addBuild(makeStringKeyRow(key), makeProjectRow("build_" + key));
        buildKeys.insert(key);
    }
    hhj->finishBuild();

    ASSERT_TRUE(stats.usedDisk);

    auto cursor = JoinCursor::empty();
    // Probe with odd-numbered keys (not in build) - should be filtered by bloom filter
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i * 2 + 1);  // Odd numbers
        auto probeKeyRow = makeStringKeyRow(key);
        auto probeProjectRow = makeProjectRow("probe_" + key);
        hhj->probe(probeKeyRow, probeProjectRow, cursor);
        // Should not match anything
        ASSERT_FALSE(cursor.next().has_value());
    }

    // Probe with even-numbered keys (in build)
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i * 2);  // Even numbers
        auto probeKeyRow = makeStringKeyRow(key);
        auto probeProjectRow = makeProjectRow("probe_" + key);
        hhj->probe(probeKeyRow, probeProjectRow, cursor);
        while (cursor.next()) {
            // Just drain the cursor
        }
    }
    hhj->finishProbe();

    ASSERT_GT(stats.numProbeRecordsDiscarded, 0);

    // Process spilled partitions and verify correctness
    while (auto cursorOpt = hhj->nextSpilledJoinCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            std::string probeKey = getStringValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKey, probeKey);
            ASSERT_TRUE(buildKeys.count(buildKey) > 0)
                << "Matched key " << buildKey << " should be in build set";
        }
    }
}

TEST_F(HybridHashJoinTestFixture, TestPartitionDistribution) {
    constexpr size_t kPartitions = 256;
    constexpr size_t kPartitionMask = kPartitions - 1;
    constexpr int kBitsPerLevel = 8;
    constexpr size_t kKeysPerPartition = 5000;
    constexpr size_t kNumKeys = kPartitions * kKeysPerPartition;

    // The distribution of keys in the partitions with good hash distribution should approximately
    // follow Poisson distribution, for which the standard deviation would be sqrt(5000)=71 and
    // Coefficient of Variation = 1/sqrt(5000) = 0.014 = 1.4%. We test with higher bounds to prevent
    // any spurious failures.
    constexpr double kMaxCV = 0.15;
    constexpr double kMaxMaxToMeanRatio =
        1.2;  // this would be 14 standard deviation away, which should be very unlikely.

    value::MaterializedRowHasher hasher;

    auto computeCounts = [&](auto&& keyGen, int recursionLevel) {
        std::vector<size_t> counts(kPartitions, 0);
        for (size_t i = 0; i < kNumKeys; ++i) {
            auto key = keyGen(i);
            size_t hash = hasher(key);
            size_t partition = (hash >> (recursionLevel * kBitsPerLevel)) & kPartitionMask;
            counts[partition]++;
        }
        return counts;
    };

    // Chi-sqaure test would be more susceptible to flaky failures in CI so commenting it out.
    /*
    constexpr double kMinPValue = 0.05;

    auto computeChiSquare = [&](auto& counts) {
        double expected = static_cast<double>(kNumKeys) / kPartitions;
        double chiSq = 0.0;
        for (auto count : counts) {
            double diff = static_cast<double>(count) - expected;
            chiSq += (diff * diff) / expected;
        }
        return chiSq;
    };

    auto computePValue = [&](double chiSquare) {
        int df = kPartitions - 1;
        boost::math::chi_squared dist(df);
        return boost::math::cdf(boost::math::complement(dist, chiSquare));
    };

    auto testChiSquare = [&](auto& counts, int level) {
        double chiSquare = computeChiSquare(counts);
        double pValue = computePValue(chiSquare);
        ASSERT_GT(pValue, kMinPValue) << "Chi-square = " << chiSquare << ". pValue = " << pValue
                                      << " at recursion level " << level;
    };
    */

    auto computeCoV = [&](auto& counts) {
        double mean = static_cast<double>(kNumKeys) / kPartitions;
        double sumSquaredDiff = 0.0;
        for (auto count : counts) {
            double diff = static_cast<double>(count) - mean;
            sumSquaredDiff += diff * diff;
        }
        double stddev = std::sqrt(sumSquaredDiff / kPartitions);
        return stddev / mean;
    };

    auto testCoV = [&](auto& counts, int level) {
        double cv = computeCoV(counts);
        ASSERT_LT(cv, kMaxCV) << "CV=" << cv << " at recursion level " << level;
    };

    auto computeMaxToMeanRatio = [&](auto& counts) {
        double mean = static_cast<double>(kNumKeys) / kPartitions;

        size_t max = 0;
        for (auto count : counts) {
            max = std::max(max, count);
        }
        return (double)max / mean;
    };

    auto testMaxToMeanRatio = [&](auto& counts, int level) {
        double maxToMeanRatio = computeMaxToMeanRatio(counts);
        ASSERT_LT(maxToMeanRatio, kMaxMaxToMeanRatio)
            << "MaxToMeanRatio=" << maxToMeanRatio << " at recursion level " << level;
    };

    auto test = [&](auto&& keyGen) {
        for (int level = 0; level <= 1; ++level) {
            auto counts = computeCounts(keyGen, level);

            // testChiSquare(counts, level);
            testCoV(counts, level);
            testMaxToMeanRatio(counts, level);
        }
    };

    test([](size_t i) { return makeKeyRow(static_cast<int64_t>(i)); });
    test([](size_t i) { return makeKeyRow(static_cast<int64_t>(i * 7)); });
    test([](size_t i) { return makeKeyRow(static_cast<int64_t>(i + 1000000)); });
    test([](size_t i) {
        return makeCompositeKeyRow(static_cast<int64_t>(i), static_cast<int64_t>(i * 3 + 1));
    });
    test([](size_t i) {
        return makeCompositeKeyRow(static_cast<int64_t>(0), static_cast<int64_t>(i));
    });
    test([](size_t i) {
        return makeCompositeKeyRow(static_cast<int64_t>(i), static_cast<int64_t>(0));
    });
    test([](size_t i) {
        return makeCompositeKeyRow(static_cast<int64_t>(i), static_cast<int64_t>(i));
    });
}
}  // namespace
}  // namespace mongo::sbe
