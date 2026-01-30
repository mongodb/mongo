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
#include <limits>
#include <map>
#include <set>
#include <string>

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
std::vector<int64_t> drainCursor(MatchCursor& cursor) {
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

std::vector<MatchTuple> drainCursorWithProjects(MatchCursor& cursor) {
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
        return std::make_unique<HybridHashJoin>(kDefaultMemLimit, collator, stats);
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
    auto cursor = hhj->probe(makeKeyRow(20), makeProjectRow("probe_20"));
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0], 20);

    // Probe with non-matching key
    auto cursor2 = hhj->probe(makeKeyRow(99), makeProjectRow("probe_99"));
    auto matches2 = drainCursor(cursor2);
    ASSERT_EQ(matches2.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, MultipleMatchesForSameKey) {
    auto hhj = makeHHJ();

    // Insert duplicates
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_a"));
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_b"));
    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42_c"));
    hhj->finishBuild();

    auto cursor = hhj->probe(makeKeyRow(42), makeProjectRow("probe_42"));

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
    (void)hhj->probe(makeKeyRow(1), makeProjectRow("probe_1"));
}

DEATH_TEST_F(HybridHashJoinDeathTest,
             PhaseGuardSpillBeforeFinishProb,
             "called nextSpilledMatchCursor() outside of kProcessSpilled phase") {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();
    (void)hhj->probe(makeKeyRow(1), makeProjectRow("probe_1"));

    // nextSpilledMatchCursor before finishProbe should fail (tassert)
    (void)hhj->nextSpilledMatchCursor();
}

TEST_F(HybridHashJoinTestFixture, EmptyBuildSide) {
    auto hhj = makeHHJ();

    hhj->finishBuild();

    auto cursor = hhj->probe(makeKeyRow(42), makeProjectRow("probe_42"));
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
    auto cursorOpt = hhj->nextSpilledMatchCursor();
    ASSERT_FALSE(cursorOpt.has_value());
}

TEST_F(HybridHashJoinTestFixture, ResetState) {
    auto hhj = makeHHJ();

    // First pass
    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->finishBuild();
    auto cursor = hhj->probe(makeKeyRow(1), makeProjectRow("probe_1"));
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 1u);
    hhj->finishProbe();

    // Reset and start over
    hhj->reset();
    ASSERT_EQ(hhj->getMemUsage(), 0u);

    // Second pass with different data
    hhj->addBuild(makeKeyRow(100), makeProjectRow("proj_100"));
    hhj->finishBuild();
    auto cursor2 = hhj->probe(makeKeyRow(100), makeProjectRow("probe_100"));
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

    auto cursor = hhj->probe(makeKeyRow(10), makeProjectRow("probe_proj_10"));
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
    for (int i = 0; i < 100; ++i) {
        auto probePayload = "probe_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow(probePayload));
        while (auto matchOpt = cursor.next()) {
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), getKeyValue(matchOpt->probeKeyRow));
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }
    hhj->finishProbe();

    // Process all spilled partitions and collect matched keys
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
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
    for (int i = 0; i < 200; ++i) {
        auto probePayload = "probe_payload_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow(probePayload));
        while (auto matchOpt = cursor.next()) {
            ASSERT_EQ(getKeyValue(matchOpt->buildKeyRow), getKeyValue(matchOpt->probeKeyRow));
            matchedKeys.insert(getKeyValue(matchOpt->buildKeyRow));
        }
    }
    hhj->finishProbe();

    // Process spilled partitions - this should trigger recursive processing
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
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

    auto cursor = hhj->probe(makeStringKeyRow("dup_key"), makeProjectRow("probe"));
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

    // Probe and collect matches (some immediate, some spilled)
    std::map<std::string, std::pair<std::string, std::string>> actualMatches;
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_" + std::to_string(i);
        std::string probeProj = "probe_proj_" + std::to_string(i);
        auto cursor = hhj->probe(makeStringKeyRow(key), makeProjectRow(probeProj));
        while (auto matchOpt = cursor.next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            std::string buildProj = getStringValue(matchOpt->buildProjectRow);
            std::string probeKey = getStringValue(matchOpt->probeKeyRow);
            std::string probeProject = getStringValue(matchOpt->probeProjectRow);
            ASSERT_EQ(buildKey, probeKey);
            actualMatches[buildKey] = {buildProj, probeProject};
        }
    }
    hhj->finishProbe();

    // Process spilled partitions
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            std::string buildKey = getStringValue(matchOpt->buildKeyRow);
            std::string buildProj = getStringValue(matchOpt->buildProjectRow);
            std::string probeKey = getStringValue(matchOpt->probeKeyRow);
            std::string probeProject = getStringValue(matchOpt->probeProjectRow);
            ASSERT_EQ(buildKey, probeKey);
            actualMatches[buildKey] = {buildProj, probeProject};
        }
    }

    // Verify all matches have correct project values
    ASSERT_EQ(actualMatches.size(), 50u);
    for (const auto& [key, projPair] : actualMatches) {
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
    auto cursor1 = hhj->probe(makeCompositeKeyRow(1, 10), makeProjectRow("probe"));
    auto matches1 = drainCursor(cursor1);
    ASSERT_EQ(matches1.size(), 1u);

    // Probe with partial match (different second column) - should not match
    auto cursor2 = hhj->probe(makeCompositeKeyRow(1, 30), makeProjectRow("probe"));
    auto matches2 = drainCursor(cursor2);
    ASSERT_EQ(matches2.size(), 0u);

    // Probe with swapped columns - should not match
    auto cursor3 = hhj->probe(makeCompositeKeyRow(10, 1), makeProjectRow("probe"));
    auto matches3 = drainCursor(cursor3);
    ASSERT_EQ(matches3.size(), 0u);
}

TEST_F(HybridHashJoinTestFixture, ProbeNonExistentKeys) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(1), makeProjectRow("proj_1"));
    hhj->addBuild(makeKeyRow(2), makeProjectRow("proj_2"));
    hhj->finishBuild();

    // Probe with keys that don't exist
    for (int i = 100; i < 110; ++i) {
        auto probePayload = "probe_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow(probePayload));
        auto matches = drainCursor(cursor);
        ASSERT_EQ(matches.size(), 0u);
    }
}

TEST_F(HybridHashJoinTestFixture, MultipleProbesForSameKey) {
    auto hhj = makeHHJ();

    hhj->addBuild(makeKeyRow(42), makeProjectRow("proj_42"));
    hhj->finishBuild();

    // Probe the same key multiple times
    for (int i = 0; i < 10; ++i) {
        auto probePayload = "probe_42_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(42), makeProjectRow(probePayload));
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
    auto cursor = hhj->probe(makeKeyRow(999), makeProjectRow("probe_999"));

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
    auto cursor = hhj->probe(makeStringKeyRow("abc"), makeProjectRow("probe_abc"));
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
    auto cursor = hhj->probe(makeStringKeyRow("xyz"), makeProjectRow("probe_xyz"));
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
    auto cursor = hhj->probe(makeStringKeyRow("anything"), makeProjectRow("probe_any"));
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
    auto cursor = hhj->probe(makeStringKeyRow("abc"), makeProjectRow("probe_abc"));
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

    for (int i = 0; i < 50; ++i) {
        (void)hhj->probe(makeKeyRow(i), makeProjectRow("probe1_" + std::to_string(i)));
    }
    hhj->finishProbe();

    // Drain spilled partitions
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
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

    auto cursor = hhj->probe(makeKeyRow(100), makeProjectRow("probe2_100"));
    auto matches = drainCursor(cursor);
    ASSERT_EQ(matches.size(), 1u);
    ASSERT_EQ(matches[0], 100);

    // Key from first pass should not exist
    auto cursor2 = hhj->probe(makeKeyRow(0), makeProjectRow("probe2_0"));
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
    for (int i = 1000; i < 1100; ++i) {
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow("probe_" + std::to_string(i)));
        ASSERT_EQ(cursor.next(), boost::none);
    }
    hhj->finishProbe();

    // Process spilled partitions - should return no matches
    size_t spilledMatches = 0;
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
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
    for (int i = 0; i < 50; ++i) {
        std::string proj = "PROBE_PROJ_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow(proj));
        if (auto matchOpt = cursor.next()) {
            int64_t buildKey = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKey = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKey, probeKey);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKey])
                << "Build project mismatch for key " << buildKey;
            ASSERT_EQ(actualProbeProj, proj) << "Probe project mismatch for key " << probeKey;

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
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            int64_t buildKey = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKey = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKey, probeKey);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKey])
                << "Build project mismatch for key " << buildKey;
            ASSERT_EQ(actualProbeProj, spilledProbeProjects[probeKey])
                << "Probe project mismatch for key " << probeKey;

            matchedKeys.insert(buildKey);
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
    for (int i = 0; i < 250; ++i) {
        std::string proj = "PROBE_PROJ_" + std::to_string(i);
        auto cursor = hhj->probe(makeKeyRow(i), makeProjectRow(proj));
        if (auto matchOpt = cursor.next()) {
            int64_t buildKey = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKey = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKey, probeKey);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKey])
                << "Build project mismatch for key " << buildKey;
            ASSERT_EQ(actualProbeProj, proj) << "Probe project mismatch for key " << probeKey;

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
    while (auto cursorOpt = hhj->nextSpilledMatchCursor()) {
        while (auto matchOpt = cursorOpt->next()) {
            int64_t buildKey = getKeyValue(matchOpt->buildKeyRow);
            int64_t probeKey = getKeyValue(matchOpt->probeKeyRow);
            ASSERT_EQ(buildKey, probeKey);

            std::string actualBuildProj = getStringValue(matchOpt->buildProjectRow);
            std::string actualProbeProj = getStringValue(matchOpt->probeProjectRow);

            // Verify the project values match what we inserted
            ASSERT_EQ(actualBuildProj, buildProjects[buildKey])
                << "Build project mismatch for key " << buildKey;
            ASSERT_EQ(actualProbeProj, spilledProbeProjects[probeKey])
                << "Probe project mismatch for key " << probeKey;

            matchedKeys.insert(buildKey);
        }
    }

    // Should have matched some keys from spilled partitions
    ASSERT_GT(matchedKeys.size(), 0u);
    ASSERT_EQ(matchedKeys.size() + inmemMatches, 250);

    // Should have swapped partitions
    ASSERT_GT(stats.numPartitionSwaps, 0);
    ASSERT_GT(stats.recursionDepthMax, 0);
}
}  // namespace
}  // namespace mongo::sbe
