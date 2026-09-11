// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::HashJoinStage.
 */

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

// Wraps a child stage and injects a saveState/restoreState on the root plan on the Nth call to
// getNext(). This simulates a storage-layer yield that fires at a precise point deep inside the
// execution tree, which cannot be reproduced by simply calling saveState()/restoreState() between
// getNext() calls from the test body.
class YieldInjectingStage final : public PlanStage {
public:
    YieldInjectingStage(std::unique_ptr<PlanStage> child, int yieldOnCallN, PlanNodeId nodeId)
        : PlanStage("yield_inject"sv, nullptr, nodeId, false), _yieldOnCallN(yieldOnCallN) {
        _children.emplace_back(std::move(child));
    }

    void setRoot(PlanStage* root) {
        _root = root;
    }

    std::unique_ptr<PlanStage> clone() const final {
        auto c = std::make_unique<YieldInjectingStage>(
            _children[0]->clone(), _yieldOnCallN, _commonStats.nodeId);
        c->_root = _root;
        return c;
    }

    void prepare(CompileCtx& ctx) final {
        _children[0]->prepare(ctx);
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        return _children[0]->getAccessor(ctx, slot);
    }

    void open(bool reOpen) final {
        _commonStats.opens++;
        _children[0]->open(reOpen);
    }

    PlanState getNext() final {
        if (++_callCount == _yieldOnCallN && _root) {
            _root->saveState();
            _root->restoreState();
        }
        return trackPlanState(_children[0]->getNext());
    }

    void close() final {
        trackClose();
        _children[0]->close();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final {
        auto ret = std::make_unique<PlanStageStats>(_commonStats);
        ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
        return ret;
    }

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    void doDebugPrint(std::vector<DebugPrinter::Block>&, DebugPrintInfo&) const final {}

    size_t estimateCompileTimeSize() const final {
        return sizeof(*this);
    }

protected:
    void doSaveState() final {}
    void doRestoreState() final {}

private:
    int _yieldOnCallN;
    int _callCount = 0;
    PlanStage* _root = nullptr;
};

using HashJoinStageTest = PlanStageTestFixture;

/**
 * Helper function to verify that actual results contain all expected results (order-independent).
 * Each result is an array of [innerKey, outerKey, innerProj, outerProj].
 */
void assertResultsMatch(value::TypeTags actualTag,
                        value::Value actualVal,
                        value::TypeTags expectedTag,
                        value::Value expectedVal) {
    ASSERT_EQ(actualTag, value::TypeTags::Array);
    ASSERT_EQ(expectedTag, value::TypeTags::Array);

    auto actualView = value::getArrayView(actualVal);
    auto expectedView = value::getArrayView(expectedVal);

    ASSERT_EQ(actualView->size(), expectedView->size())
        << "Result size mismatch: got " << actualView->size() << ", expected "
        << expectedView->size();

    // For each expected result, verify it exists in actual results
    for (size_t i = 0; i < expectedView->size(); i++) {
        auto [expectedRowTag, expectedRowVal] = expectedView->getAt(i);
        bool found = false;

        for (size_t j = 0; j < actualView->size(); j++) {
            auto [actualRowTag, actualRowVal] = actualView->getAt(j);
            auto [cmpTag, cmpVal] =
                value::compareValue(expectedRowTag, expectedRowVal, actualRowTag, actualRowVal);
            if (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0) {
                found = true;
                break;
            }
        }

        ASSERT_TRUE(found) << "Expected result not found at index " << i;
    }
}

/**
 * Helper function to create a HashJoinStage for testing.
 * This sets up a hash join where the inner side builds the hash table and
 * the outer side probes it.
 */
std::pair<value::SlotVector, std::unique_ptr<PlanStage>> makeHashJoinStage(
    PlanStageTestFixture* fixture,
    std::unique_ptr<PlanStage> outerStage,
    std::unique_ptr<PlanStage> innerStage,
    value::SlotVector outerCondSlots,
    value::SlotVector outerProjectSlots,
    value::SlotVector innerCondSlots,
    value::SlotVector innerProjectSlots,
    boost::optional<value::SlotId> collatorSlot = boost::none,
    bool allowDiskUse = true) {

    // Output slots are the condition and project slots from both sides
    value::SlotVector outputSlots;
    outputSlots.insert(outputSlots.end(), innerCondSlots.begin(), innerCondSlots.end());
    outputSlots.insert(outputSlots.end(), outerCondSlots.begin(), outerCondSlots.end());
    outputSlots.insert(outputSlots.end(), innerProjectSlots.begin(), innerProjectSlots.end());
    outputSlots.insert(outputSlots.end(), outerProjectSlots.begin(), outerProjectSlots.end());

    auto hashJoinStage = makeS<HashJoinStage>(std::move(outerStage),
                                              std::move(innerStage),
                                              outerCondSlots,
                                              outerProjectSlots,
                                              innerCondSlots,
                                              innerProjectSlots,
                                              collatorSlot,
                                              allowDiskUse,
                                              nullptr /* yieldPolicy */,
                                              kEmptyPlanNodeId,
                                              boost::none);

    return std::make_pair(std::move(outputSlots), std::move(hashJoinStage));
}

TEST_F(HashJoinStageTest, BasicHashJoin) {
    auto ctx = makeCompileCtx();

    // Outer side (probes hash table): [[key, proj], ...] = [[1, "big_string_1"], [2,
    // "big_string_1"], [3, "big_string3"]]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "big_string_1") << BSON_ARRAY(2 << "big_string_2")
                                                             << BSON_ARRAY(3 << "big_string_3"))));

    // Inner side (builds hash table): [[key, proj], ...] = [[2, "big_string_x"], [3,
    // "big_string_y"], [4, "big_string_z"]]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(2 << "big_string_x") << BSON_ARRAY(3 << "big_string_y")
                                                             << BSON_ARRAY(4 << "big_string_z"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    // Get all results
    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Key 2 matches: [2, 2, "big_string_x", "big_string_2"]
    // Key 3 matches: [3, 3, "big_string_y", "big_string_3"]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(2 << 2 << "big_string_x" << "big_string_2")
                             << BSON_ARRAY(3 << 3 << "big_string_y" << "big_string_3"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, SpillThrowsWhenDiskUseNotAllowed) {
    // Force the hash table to spill by setting a tiny memory limit.
    unittest::ServerParameterGuard maxMemoryLimit(
        "internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill",
        static_cast<long long>(16));

    auto ctx = makeCompileCtx();

    // Build enough inner rows that the hash table exceeds the tiny memory limit.
    BSONArrayBuilder innerBuilder;
    for (int i = 0; i < 50; ++i) {
        innerBuilder.append(BSON_ARRAY(i << ("big_string_" + std::to_string(i))));
    }
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(innerBuilder.arr()));
    value::TagValueOwned outerArr =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(BSON_ARRAY(1 << "x"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);

    // Construct the join with allowDiskUse = false.
    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerSlots[0]),
                                                          makeSV(outerSlots[1]),
                                                          makeSV(innerSlots[0]),
                                                          makeSV(innerSlots[1]),
                                                          boost::none /* collatorSlot */,
                                                          false /* allowDiskUse */);

    // The stage must throw QueryExceededMemoryLimitNoDiskUseAllowed when it tries to spill during
    // the build phase, instead of spilling.
    ASSERT_THROWS_CODE(
        [&] {
            auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);
            getAllResultsMulti(hashJoinStage.get(), resultAccessors);
        }(),
        DBException,
        ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(HashJoinStageTest, HashJoinEmptyOuter) {
    auto ctx = makeCompileCtx();

    // Empty outer side
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSONArray()));

    // Inner side with values: [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b") << BSON_ARRAY(3 << "c"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // No matches when outer is empty - expect empty array
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(BSONArray()));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinEmptyInner) {
    auto ctx = makeCompileCtx();

    // Outer side with values: [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << 100) << BSON_ARRAY(2 << 200) << BSON_ARRAY(3 << 300))));

    // Empty inner side
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(BSONArray()));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // No matches when inner is empty - expect empty array
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(BSONArray()));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinDuplicateKeys) {
    auto ctx = makeCompileCtx();

    // Outer side with duplicate keys: [[key, proj], ...] = [[1, "a"], [1, "b"], [2, "c"]]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(2 << "c"))));

    // Inner side with duplicate keys: [[key, proj], ...] = [[1, 10], [2, 20], [2, 30]]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(2 << 20) << BSON_ARRAY(2 << 30))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 1 << 10 << "a")
                             << BSON_ARRAY(1 << 1 << 10 << "b") << BSON_ARRAY(2 << 2 << 20 << "c")
                             << BSON_ARRAY(2 << 2 << 30 << "c"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinNoMatches) {
    auto ctx = makeCompileCtx();

    // Outer side: [[key, proj], ...] = [[1, "a"], [2, "b"], [3, "c"]]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b") << BSON_ARRAY(3 << "c"))));

    // Inner side: keys 4, 5, 6 (no overlap) [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(4 << "x") << BSON_ARRAY(5 << "y") << BSON_ARRAY(6 << "z"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // No matches since keys don't overlap - expect empty array
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(BSONArray()));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinStringKeys) {
    auto ctx = makeCompileCtx();

    // Outer side: string keys with projections [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY("apple" << 1) << BSON_ARRAY("banana" << 2) << BSON_ARRAY("cherry" << 3))));

    // Inner side: string keys with projections [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY("banana" << "b") << BSON_ARRAY("cherry" << "c") << BSON_ARRAY("date" << "d"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // "banana" matches: ["banana", "banana", "b", 2]
    // "cherry" matches: ["cherry", "cherry", "c", 3]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("banana" << "banana" << "b" << 2)
                             << BSON_ARRAY("cherry" << "cherry" << "c" << 3))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinMixedTypes) {
    auto ctx = makeCompileCtx();

    // Outer side: mixed integer and double values that are equal [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << "int1") << BSON_ARRAY(2.0 << "dbl2") << BSON_ARRAY(3 << "int3"))));

    // Inner side: values that should match [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1.0 << 100) << BSON_ARRAY(2 << 200) << BSON_ARRAY(4 << 400))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // 1.0 (inner) matches 1 (outer): [1.0, 1, 100, "int1"]
    // 2 (inner) matches 2.0 (outer): [2, 2.0, 200, "dbl2"]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1.0 << 1 << 100 << "int1") << BSON_ARRAY(2 << 2.0 << 200 << "dbl2"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinSingleMatch) {
    auto ctx = makeCompileCtx();

    // Single element on each side that matches [[key, proj]]
    value::TagValueOwned outerArr =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(BSON_ARRAY(42 << "outer"))));

    value::TagValueOwned innerArr =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(BSON_ARRAY(42 << "inner"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Single match: [42, 42, "inner", "outer"]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(42 << 42 << "inner" << "outer"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinManyDuplicatesOnOuter) {
    auto ctx = makeCompileCtx();

    // Many duplicates on outer side with different projections [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "c")
                                        << BSON_ARRAY(1 << "d") << BSON_ARRAY(1 << "e"))));

    // Single match on inner side [[key, proj]]
    value::TagValueOwned innerArr =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(BSON_ARRAY(1 << 100))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Inner (key=1, proj=100) matches all 5 outer rows with different projections
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << 1 << 100 << "a")
                   << BSON_ARRAY(1 << 1 << 100 << "b") << BSON_ARRAY(1 << 1 << 100 << "c")
                   << BSON_ARRAY(1 << 1 << 100 << "d") << BSON_ARRAY(1 << 1 << 100 << "e"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinManyDuplicatesOnInner) {
    auto ctx = makeCompileCtx();

    // Single probe on outer [[key, proj]]
    value::TagValueOwned outerArr =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(BSON_ARRAY(1 << "big_outer_string"))));

    // Many matches from inner with same key but different projections [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << "big_inner_string_10")
        << BSON_ARRAY(1 << "big_inner_string_20") << BSON_ARRAY(1 << "big_inner_string_30")
        << BSON_ARRAY(1 << "big_inner_string_40") << BSON_ARRAY(1 << "big_inner_string_50"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // All 5 inner rows (each with different proj) match the single outer row
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << 1 << "big_inner_string_10" << "big_outer_string")
                   << BSON_ARRAY(1 << 1 << "big_inner_string_20" << "big_outer_string")
                   << BSON_ARRAY(1 << 1 << "big_inner_string_30" << "big_outer_string")
                   << BSON_ARRAY(1 << 1 << "big_inner_string_40" << "big_outer_string")
                   << BSON_ARRAY(1 << 1 << "big_inner_string_50" << "big_outer_string"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinCartesianProduct) {
    auto ctx = makeCompileCtx();

    // 3 duplicates on outer with different projections [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "c"))));

    // 4 probes on inner with same key but different projections [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3) << BSON_ARRAY(1 << 4))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Cartesian product: 4 inner rows x 3 outer rows = 12 results
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 1 << 1 << "a")
                             << BSON_ARRAY(1 << 1 << 1 << "b") << BSON_ARRAY(1 << 1 << 1 << "c")
                             << BSON_ARRAY(1 << 1 << 2 << "a") << BSON_ARRAY(1 << 1 << 2 << "b")
                             << BSON_ARRAY(1 << 1 << 2 << "c") << BSON_ARRAY(1 << 1 << 3 << "a")
                             << BSON_ARRAY(1 << 1 << 3 << "b") << BSON_ARRAY(1 << 1 << 3 << "c")
                             << BSON_ARRAY(1 << 1 << 4 << "a") << BSON_ARRAY(1 << 1 << 4 << "b")
                             << BSON_ARRAY(1 << 1 << 4 << "c"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinNullKeys) {
    auto ctx = makeCompileCtx();

    // Outer with null key [[key, proj], ...]
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(BSONNULL << "null") << BSON_ARRAY(1 << "one") << BSON_ARRAY(2 << "two"))));

    // Inner with null key [[key, proj], ...]
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY(BSONNULL << 0) << BSON_ARRAY(2 << 200) << BSON_ARRAY(3 << 300))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // null-null match: [null, null, 0, "null"]
    // 2-2 match: [2, 2, 200, "two"]
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(BSONNULL << BSONNULL << 0 << "null") << BSON_ARRAY(2 << 2 << 200 << "two"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

TEST_F(HashJoinStageTest, HashJoinCollationTest) {
    using namespace std::literals;
    for (auto useCollator : {false, true}) {
        // Inner side: [[key, proj], ...]
        value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(
            BSON_ARRAY(BSON_ARRAY("a" << 1) << BSON_ARRAY("b" << 2) << BSON_ARRAY("c" << 3))));

        // Outer side: [[key, proj], ...]
        value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
            BSON_ARRAY("a" << "x") << BSON_ARRAY("b" << "y") << BSON_ARRAY("A" << "z"))));

        auto collatorSlot = generateSlotId();

        auto makeStageFn =
            [this, collatorSlot, useCollator](value::SlotId outerCondSlot,
                                              value::SlotId outerProjSlot,
                                              value::SlotId innerCondSlot,
                                              value::SlotId innerProjSlot,
                                              std::unique_ptr<PlanStage> outerStage,
                                              std::unique_ptr<PlanStage> innerStage) {
                auto hashJoinStage =
                    makeS<HashJoinStage>(std::move(outerStage),
                                         std::move(innerStage),
                                         makeSV(outerCondSlot),
                                         makeSV(outerProjSlot),
                                         makeSV(innerCondSlot),
                                         makeSV(innerProjSlot),
                                         boost::optional<value::SlotId>{useCollator, collatorSlot},
                                         true /* allowDiskUse */,
                                         nullptr /* yieldPolicy */,
                                         kEmptyPlanNodeId,
                                         boost::none);

                return std::make_pair(
                    makeSV(innerCondSlot, outerCondSlot, innerProjSlot, outerProjSlot),
                    std::move(hashJoinStage));
            };

        auto ctx = makeCompileCtx();

        // Setup collator and insert it into the ctx.
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        value::OwnedValueAccessor collatorAccessor;
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Two separate virtual scans are needed since HashJoinStage needs two child stages.
        auto [outerT, outerV] = outerArr.releaseToRaw();
        auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);
        auto outerCondSlot = outerSlots[0];
        auto outerProjSlot = outerSlots[1];

        auto [innerT, innerV] = innerArr.releaseToRaw();
        auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);
        auto innerCondSlot = innerSlots[0];
        auto innerProjSlot = innerSlots[1];

        // Call the `makeStage` callback to create the HashJoinStage, passing in the mock scan
        // subtrees and the subtree's output slots.
        auto [outputSlots, stage] = makeStageFn(outerCondSlot,
                                                outerProjSlot,
                                                innerCondSlot,
                                                innerProjSlot,
                                                std::move(outerStage),
                                                std::move(innerStage));

        // Prepare the tree and get the SlotAccessor for the output slots.
        auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

        // Get all the results produced by HashJoin.
        value::TagValueOwned resultsArr =
            value::TagValueOwned::fromRaw(getAllResultsMulti(stage.get(), resultAccessors));

        // Expected results: [innerKey, outerKey, innerProj, outerProj]
        // With collator: "a" matches "a" and "A" (case-insensitive), "b" matches "b"
        // Without collator: "a" matches "a", "b" matches "b"
        value::TypeTags expectedTag;
        value::Value expectedVal;
        if (useCollator) {
            std::tie(expectedTag, expectedVal) = makeArray(BSON_ARRAY(
                BSON_ARRAY("a" << "a" << 1 << "x")
                << BSON_ARRAY("a" << "A" << 1 << "z") << BSON_ARRAY("b" << "b" << 2 << "y")));
        } else {
            std::tie(expectedTag, expectedVal) = makeArray(BSON_ARRAY(
                BSON_ARRAY("a" << "a" << 1 << "x") << BSON_ARRAY("b" << "b" << 2 << "y")));
        }
        value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(expectedTag, expectedVal);

        assertResultsMatch(
            resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
    }
}

TEST_F(HashJoinStageTest, HashJoinMultipleJoinKeys) {
    auto ctx = makeCompileCtx();

    // Outer side: rows are [key1, key2, proj]. Join on (key1, key2).
    // (1,10), (1,20), (2,10)
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 10 << "a") << BSON_ARRAY(1 << 20 << "b") << BSON_ARRAY(2 << 10 << "c"))));

    // Inner side: rows are [key1, key2, proj]. (1,10), (2,10), (2,20)
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 10 << "x") << BSON_ARRAY(2 << 10 << "y") << BSON_ARRAY(2 << 20 << "z"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(3, outerT, outerV);
    auto outerCondSlot0 = outerSlots[0];
    auto outerCondSlot1 = outerSlots[1];
    auto outerProjSlot = outerSlots[2];

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(3, innerT, innerV);
    auto innerCondSlot0 = innerSlots[0];
    auto innerCondSlot1 = innerSlots[1];
    auto innerProjSlot = innerSlots[2];

    // Two condition slots per side: join on (key1, key2) matching inner and outer.
    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot0, outerCondSlot1),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot0, innerCondSlot1),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));

    // Output order: innerCond (key1, key2), outerCond (key1, key2), innerProj, outerProj.
    // (1,10) matches: [1, 10, 1, 10, "x", "a"]
    // (2,10) matches: [2, 10, 2, 10, "y", "c"]
    // (1,20) and (2,20) have no match on the other side.
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 10 << 1 << 10 << "x" << "a")
                             << BSON_ARRAY(2 << 10 << 2 << 10 << "y" << "c"))));

    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
}

// Wraps the outer (probe) child of a HashJoinStage and hands out each row's values in freshly
// allocated buffers, freeing the previous row's buffers as it advances. It also injects a
// saveState()/restoreState() on the root plan on the Nth call to getNext(), after the previous
// row's buffers have been freed. This simulates a yield firing inside the probe child's getNext()
// while the hash join's probe rows still hold views into the freed buffers.
class BufferRecyclingYieldStage final : public PlanStage {
public:
    BufferRecyclingYieldStage(std::unique_ptr<PlanStage> child,
                              value::SlotVector slots,
                              int yieldOnCallN,
                              PlanNodeId nodeId)
        : PlanStage("buffer_recycle"sv, nullptr, nodeId, false),
          _slots(std::move(slots)),
          _outAccessors(_slots.size()),
          _yieldOnCallN(yieldOnCallN) {
        _children.emplace_back(std::move(child));
    }

    void setRoot(PlanStage* root) {
        _root = root;
    }

    // Frees the buffers backing the values handed out for the current row, simulating the
    // underlying storage having been released.
    void releaseCurrentRowBuffersForTest() {
        for (auto& accessor : _outAccessors) {
            accessor.reset();
        }
    }

    std::unique_ptr<PlanStage> clone() const final {
        auto c = std::make_unique<BufferRecyclingYieldStage>(
            _children[0]->clone(), _slots, _yieldOnCallN, _commonStats.nodeId);
        c->_root = _root;
        return c;
    }

    void prepare(CompileCtx& ctx) final {
        _children[0]->prepare(ctx);
        for (auto slot : _slots) {
            _inAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        }
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        for (size_t i = 0; i < _slots.size(); ++i) {
            if (_slots[i] == slot) {
                return &_outAccessors[i];
            }
        }
        return ctx.getAccessor(slot);
    }

    void open(bool reOpen) final {
        _commonStats.opens++;
        _children[0]->open(reOpen);
    }

    PlanState getNext() final {
        auto state = _children[0]->getNext();
        if (state == PlanState::ADVANCED) {
            // Copy the child's values into freshly allocated buffers; resetting the accessor
            // frees the buffers handed out for the previous row, as a storage cursor would on
            // advancing.
            for (size_t i = 0; i < _inAccessors.size(); ++i) {
                _outAccessors[i].reset(_inAccessors[i]->getViewOfValue().copy());
            }
        }
        if (++_callCount == _yieldOnCallN && _root) {
            _root->saveState();
            _root->restoreState();
        }
        return trackPlanState(state);
    }

    void close() final {
        trackClose();
        _children[0]->close();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final {
        auto ret = std::make_unique<PlanStageStats>(_commonStats);
        ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
        return ret;
    }

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    void doDebugPrint(std::vector<DebugPrinter::Block>&, DebugPrintInfo&) const final {}

    size_t estimateCompileTimeSize() const final {
        return sizeof(*this);
    }

protected:
    // The values handed out are owned by _outAccessors, so they are stable across yields.
    void doSaveState() final {}
    void doRestoreState() final {}

private:
    value::SlotVector _slots;
    std::vector<value::SlotAccessor*> _inAccessors;
    std::vector<value::OwnedValueAccessor> _outAccessors;
    int _yieldOnCallN;
    int _callCount = 0;
    PlanStage* _root = nullptr;
};

// While HashJoinStage advances its outer (probe) child, its _probeKey/_probeProject rows still
// hold views into the previous outer row's buffers, which the child may have already freed.
// If a yield fires at that point, the join must not try to deep-copy those stale views into its
// join cursor's saved state.
TEST_F(HashJoinStageTest, NoUseAfterFreeWhenYieldFiresWhileAdvancingOuterChild) {
    auto ctx = makeCompileCtx();

    // Use strings > 7 bytes so values are heap-allocated (StringBig). Both outer rows
    // match the single build row, so the row produced after the mid-advance yield is
    // also verified.
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "outer_proj_1_long")
                             << BSON_ARRAY("k1_long_key_string" << "outer_proj_2_long"))));

    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "inner_proj_long"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outerT, outerV);

    // The yield fires on call 2 of the wrapper's getNext(): row 1's buffers have just been freed
    // (row 2 was copied into fresh ones), while the join's probe rows still view row 1's buffers.
    auto recyclingStage = std::make_unique<BufferRecyclingYieldStage>(
        std::move(outerScanStage), outerScanSlots, 2 /* yieldOnCallN */, kEmptyPlanNodeId);
    auto* recyclingStagePtr = recyclingStage.get();

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, innerT, innerV);

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(recyclingStage),
                                                          std::move(innerScanStage),
                                                          makeSV(outerScanSlots[0]),
                                                          makeSV(outerScanSlots[1]),
                                                          makeSV(innerScanSlots[0]),
                                                          makeSV(innerScanSlots[1]));

    // Output slot order: innerCond, outerCond, innerProj, outerProj.
    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);
    recyclingStagePtr->setRoot(hashJoinStage.get());

    auto assertStrSlot = [](value::SlotAccessor* acc, std::string_view expected) {
        auto [tag, val] = acc->getViewOfValue();
        ASSERT_TRUE(value::isString(tag));
        ASSERT_EQ(value::getStringView(tag, val), expected);
    };

    // Match 1: outer row 1.
    ASSERT_EQ(hashJoinStage->getNext(), PlanState::ADVANCED);
    assertStrSlot(resultAccessors[3], "outer_proj_1_long");

    // Match 2: fetching outer row 2 frees row 1's buffers and then fires the yield.
    ASSERT_EQ(hashJoinStage->getNext(), PlanState::ADVANCED);
    assertStrSlot(resultAccessors[3], "outer_proj_2_long");

    ASSERT_EQ(hashJoinStage->getNext(), PlanState::IS_EOF);
    hashJoinStage->close();
}

// When HashJoinStage is re-opened, a cursor left over from the previous execution must be
// dropped before the build phase runs: the cursor references _probeKey/_probeProject, which may
// still view buffers that no longer exist, so a yield fired by the build child during the
// re-opened build phase must not save them.
TEST_F(HashJoinStageTest, NoUseAfterFreeWhenYieldFiresDuringReopenedBuildPhase) {
    auto ctx = makeCompileCtx();

    // Strings longer than 7 bytes are heap-allocated (StringBig). Outer row 1 has two build-side
    // matches, so the join cursor is still streaming the row's matches (i.e. is still alive) when
    // the stage is re-opened below.
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "outer_proj_1_long")
                             << BSON_ARRAY("k2_long_key_string" << "outer_proj_2_long"))));

    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "inner_proj_a_long")
                             << BSON_ARRAY("k1_long_key_string" << "inner_proj_b_long")
                             << BSON_ARRAY("k2_long_key_string" << "inner_proj_c_long"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outerT, outerV);

    constexpr int kNeverYield = -1;
    auto recyclingStage = std::make_unique<BufferRecyclingYieldStage>(
        std::move(outerScanStage), outerScanSlots, kNeverYield, kEmptyPlanNodeId);
    auto* recyclingStagePtr = recyclingStage.get();

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, innerT, innerV);

    // The first build phase consumes four inner getNext() calls (three rows plus EOF), so the
    // injected yield fires on the first getNext() of the re-opened build phase.
    auto yieldStage = std::make_unique<YieldInjectingStage>(
        std::move(innerScanStage), 5 /* yieldOnCallN */, kEmptyPlanNodeId);
    auto* yieldStagePtr = yieldStage.get();

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(recyclingStage),
                                                          std::move(yieldStage),
                                                          makeSV(outerScanSlots[0]),
                                                          makeSV(outerScanSlots[1]),
                                                          makeSV(innerScanSlots[0]),
                                                          makeSV(innerScanSlots[1]));

    // Output slot order: innerCond, outerCond, innerProj, outerProj.
    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);
    recyclingStagePtr->setRoot(hashJoinStage.get());
    yieldStagePtr->setRoot(hashJoinStage.get());

    // First execution: produce the first match of outer row 1. The cursor retains a pending
    // match ("inner_proj_b_long") and the probe rows view the outer wrapper's row-1 buffers.
    ASSERT_EQ(hashJoinStage->getNext(), PlanState::ADVANCED);
    auto assertStrSlot = [](value::SlotAccessor* acc, std::string_view expected) {
        auto [tag, val] = acc->getViewOfValue();
        ASSERT_TRUE(value::isString(tag));
        ASSERT_EQ(value::getStringView(tag, val), expected);
    };
    assertStrSlot(resultAccessors[3], "outer_proj_1_long");

    // Simulate the outer side's storage having been released while the stage was idle: the
    // buffers backing the probe-row views are freed.
    recyclingStagePtr->releaseCurrentRowBuffersForTest();

    // Re-open the stage: The stage must drop the leftover cursor and disable slot access up
    // front, so that the yield fired by the build child's first getNext() does not save the
    // dangling probe rows.
    hashJoinStage->open(true /* reOpen */);

    // The re-opened join produces the full result set from scratch.
    value::TagValueOwned resultsArr =
        value::TagValueOwned::fromRaw(getAllResultsMulti(hashJoinStage.get(), resultAccessors));
    value::TagValueOwned expectedArr = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "k1_long_key_string" << "inner_proj_a_long"
                                                   << "outer_proj_1_long")
                   << BSON_ARRAY("k1_long_key_string" << "k1_long_key_string"
                                                      << "inner_proj_b_long"
                                                      << "outer_proj_1_long")
                   << BSON_ARRAY("k2_long_key_string" << "k2_long_key_string"
                                                      << "inner_proj_c_long"
                                                      << "outer_proj_2_long"))));
    assertResultsMatch(
        resultsArr.tag(), resultsArr.value(), expectedArr.tag(), expectedArr.value());
    hashJoinStage->close();
}

// Test for nested HashJoinStage use-after-free on yield.
//
// ChildHashJoin._probeProject holds a non-owned VIEW into ParentHashJoin's _probeProject (HJ_1).
// When ParentHashJoin advances to the next outer row it frees HJ_1 via _probeProject.reset().
// ChildHashJoin._probeProject is now dangling. If a storage-layer yield fires before
// ChildHashJoin gets a new outer row, doSaveState() calls _probeProject.makeOwned() →
// copyValue() → memcpy on the freed buffer → heap-use-after-free.
TEST_F(HashJoinStageTest, NestedHashJoinNoUseAfterFreeOnYield) {
    auto ctx = makeCompileCtx();

    // Use strings > 7 bytes so values are heap-allocated (StringBig). StringSmall is stored
    // inline in the Value word and is never freed to the heap, so it cannot exhibit a UaF.

    // scan1: outer (probe) side of ParentHashJoin.
    //   Row 1: "k1_long_key" — 2 matches in scan2 (keeps cursor alive across the safe yield).
    //   Row 2: "k_no_match_long" — no match; this row causes ParentHashJoin to free HJ_1.
    value::TagValueOwned scan1Arr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key" << "proj_A_long")
                             << BSON_ARRAY("k_no_match_long" << "proj_B_long"))));

    // scan2: inner (build) side of ParentHashJoin.
    value::TagValueOwned scan2Arr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key" << "inner_1_long")
                             << BSON_ARRAY("k1_long_key" << "inner_2_long"))));

    // scan3: inner (build) side of ChildHashJoin.
    value::TagValueOwned scan3Arr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key" << "child_1_long"))));

    auto [scan1T, scan1V] = scan1Arr.releaseToRaw();
    auto [scan1Slots, scan1Stage] = generateVirtualScanMulti(2, scan1T, scan1V);

    // YieldInjectingStage fires on call 3 of scan1.getNext():
    //   Call 1 → "k1_long_key"      (Match 1 and Match 2 setup)
    //   Call 2 → "k_no_match_long"  (ParentHashJoin frees HJ_1; ChildHashJoin._probeProject
    //                                dangles)
    //   Call 3 → IS_EOF             (yield fires: saveState() → makeOwned() on dangling ptr)
    auto yieldStage = std::make_unique<YieldInjectingStage>(
        std::move(scan1Stage), 3 /* yieldOnCallN */, kEmptyPlanNodeId);
    auto* yieldStagePtr = yieldStage.get();

    auto [scan2T, scan2V] = scan2Arr.releaseToRaw();
    auto [scan2Slots, scan2Stage] = generateVirtualScanMulti(2, scan2T, scan2V);

    auto [parentOutputSlots, parentStage] = makeHashJoinStage(this,
                                                              std::move(yieldStage),
                                                              std::move(scan2Stage),
                                                              makeSV(scan1Slots[0]),
                                                              makeSV(scan1Slots[1]),
                                                              makeSV(scan2Slots[0]),
                                                              makeSV(scan2Slots[1]));

    auto childOuterKeySlot = parentOutputSlots[1];
    value::SlotVector childOuterProjSlots = {
        parentOutputSlots[0], parentOutputSlots[2], parentOutputSlots[3]};

    auto [scan3T, scan3V] = scan3Arr.releaseToRaw();
    auto [scan3Slots, scan3Stage] = generateVirtualScanMulti(2, scan3T, scan3V);

    auto [childOutputSlots, childStage] = makeHashJoinStage(this,
                                                            std::move(parentStage),
                                                            std::move(scan3Stage),
                                                            makeSV(childOuterKeySlot),
                                                            childOuterProjSlots,
                                                            makeSV(scan3Slots[0]),
                                                            makeSV(scan3Slots[1]));

    prepareTree(ctx.get(), childStage.get(), childOutputSlots);
    yieldStagePtr->setRoot(childStage.get());

    // Match 1: "k1_long_key" → inner_1.
    ASSERT_EQ(childStage->getNext(), PlanState::ADVANCED);

    // Safe yield: both stages make owned copies of their probe rows.
    //   ParentHashJoin: HJ_1 = owned copy of "proj_A_long".
    //   ChildHashJoin:  HJ_2 = owned copy of "proj_A_long".
    childStage->saveState();
    childStage->restoreState();

    // Match 2: ParentHashJoin cursor advances to inner_2 (cursor was still live from Match 1).
    //   ChildHashJoin._probeProject.reset() releases HJ_2 and loads a VIEW into HJ_1.
    ASSERT_EQ(childStage->getNext(), PlanState::ADVANCED);

    // This getNext() triggers the bug:
    //   - ChildHashJoin cursor exhausted → calls ParentHashJoin->getNext()
    //   - ParentHashJoin cursor exhausted → calls YieldInjectingStage (call 2)
    //     → returns "k_no_match_long" → ParentHashJoin._probeProject.reset() frees HJ_1
    //     → ChildHashJoin._probeProject[2] is now a dangling pointer
    //   - ParentHashJoin: no match → calls YieldInjectingStage (call 3)
    //     → yield fires: saveState() → ChildHashJoin.doSaveState() → makeOwned()
    //     → copyValue(StringBig, dangling_ptr) → heap-use-after-free (caught by ASAN)
    ASSERT_EQ(childStage->getNext(), PlanState::IS_EOF);
    childStage->close();
}

// Variant of BufferRecyclingYieldStage that yields (root saveState()/restoreState()) on every
// k-th getNext() call.
class RecyclingYieldEveryStage final : public PlanStage {
public:
    RecyclingYieldEveryStage(std::unique_ptr<PlanStage> child,
                             value::SlotVector slots,
                             int yieldEveryN,
                             PlanNodeId nodeId)
        : PlanStage("buffer_recycle_every"sv, nullptr, nodeId, false),
          _slots(std::move(slots)),
          _outAccessors(_slots.size()),
          _yieldEveryN(yieldEveryN) {
        _children.emplace_back(std::move(child));
    }

    void setRoot(PlanStage* root) {
        _root = root;
    }

    void releaseCurrentRowBuffersForTest() {
        for (auto& accessor : _outAccessors) {
            accessor.reset();
        }
    }

    // Arms the next restoreState() to fail with a WriteConflictException, simulating a storage
    // restore failure that triggers PlanYieldPolicy's save/yield/restore retry.
    void setThrowOnNextRestoreForTest() {
        _throwOnNextRestore = true;
    }

    std::unique_ptr<PlanStage> clone() const final {
        auto c = std::make_unique<RecyclingYieldEveryStage>(
            _children[0]->clone(), _slots, _yieldEveryN, _commonStats.nodeId);
        c->_root = _root;
        return c;
    }

    void prepare(CompileCtx& ctx) final {
        _children[0]->prepare(ctx);
        for (auto slot : _slots) {
            _inAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        }
    }

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final {
        for (size_t i = 0; i < _slots.size(); ++i) {
            if (_slots[i] == slot) {
                return &_outAccessors[i];
            }
        }
        return ctx.getAccessor(slot);
    }

    void open(bool reOpen) final {
        _commonStats.opens++;
        _children[0]->open(reOpen);
    }

    PlanState getNext() final {
        auto state = _children[0]->getNext();
        if (state == PlanState::ADVANCED) {
            // Copy the child's values into freshly allocated buffers; resetting the accessor
            // frees the buffers handed out for the previous row, as a storage cursor would on
            // advancing.
            for (size_t i = 0; i < _inAccessors.size(); ++i) {
                _outAccessors[i].reset(_inAccessors[i]->getViewOfValue().copy());
            }
        }
        if (++_callCount % _yieldEveryN == 0 && _root) {
            _root->saveState();
            _root->restoreState();
        }
        return trackPlanState(state);
    }

    void close() final {
        trackClose();
        _children[0]->close();
    }

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final {
        auto ret = std::make_unique<PlanStageStats>(_commonStats);
        ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
        return ret;
    }

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

    void doDebugPrint(std::vector<DebugPrinter::Block>&, DebugPrintInfo&) const final {}

    size_t estimateCompileTimeSize() const final {
        return sizeof(*this);
    }

protected:
    // The values handed out are owned by _outAccessors, so they are stable across yields.
    void doSaveState() final {}
    void doRestoreState() final {
        if (std::exchange(_throwOnNextRestore, false)) {
            uasserted(ErrorCodes::WriteConflict, "simulated write conflict on restore");
        }
    }

private:
    value::SlotVector _slots;
    std::vector<value::SlotAccessor*> _inAccessors;
    std::vector<value::OwnedValueAccessor> _outAccessors;
    int _yieldEveryN;
    int _callCount = 0;
    PlanStage* _root = nullptr;
    bool _throwOnNextRestore = false;
};

// When a yield's restoreState() fails (e.g. a storage-layer WriteConflict), the yield policy
// retries the whole yield starting with a fresh saveState().
TEST_F(HashJoinStageTest, NoUseAfterFreeOnYieldRetryAfterFailedRestore) {
    auto ctx = makeCompileCtx();

    // Outer row 1 has two build-side matches, so the join cursor still has a pending match when
    // the simulated yield begins. Long strings are heap-allocated (StringBig).
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "outer_proj_1_long")
                             << BSON_ARRAY("k2_long_key_string" << "outer_proj_2_long"))));
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("k1_long_key_string" << "inner_proj_a_long")
                             << BSON_ARRAY("k1_long_key_string" << "inner_proj_b_long")
                             << BSON_ARRAY("k2_long_key_string" << "inner_proj_c_long"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerScanSlots, outerScanStage] = generateVirtualScanMulti(2, outerT, outerV);

    auto recyclingStage = std::make_unique<RecyclingYieldEveryStage>(
        std::move(outerScanStage), outerScanSlots, 1000000 /* never yields */, kEmptyPlanNodeId);
    auto* recyclingStagePtr = recyclingStage.get();

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerScanSlots, innerScanStage] = generateVirtualScanMulti(2, innerT, innerV);

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(recyclingStage),
                                                          std::move(innerScanStage),
                                                          makeSV(outerScanSlots[0]),
                                                          makeSV(outerScanSlots[1]),
                                                          makeSV(innerScanSlots[0]),
                                                          makeSV(innerScanSlots[1]));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    // Produce the first match of outer row 1; the cursor retains a pending match and the probe
    // rows view the outer wrapper's row-1 buffers.
    ASSERT_EQ(hashJoinStage->getNext(), PlanState::ADVANCED);

    // Yield attempt 1: save, then the storage release frees the row-1 buffers, then the restore
    // fails in the outer child.
    hashJoinStage->saveState();
    recyclingStagePtr->releaseCurrentRowBuffersForTest();
    recyclingStagePtr->setThrowOnNextRestoreForTest();
    ASSERT_THROWS_CODE(hashJoinStage->restoreState(), DBException, ErrorCodes::WriteConflict);

    // The yield policy retries with a fresh save; the already-owned probe rows must be untouched
    // by it.
    hashJoinStage->saveState();
    hashJoinStage->restoreState();

    // Drain the rest of the results to keep the stage in a consistent state for close().
    while (hashJoinStage->getNext() == PlanState::ADVANCED) {
    }
    hashJoinStage->close();
}

TEST_F(HashJoinStageTest, NoUseAfterFreeWhenYieldFiresAfterMidStreamReopenBeforeFirstProbe) {
    auto ctx = makeCompileCtx();

    // Use long (heap-allocated) strings so views into freed buffers are detectable.
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("key_string_1_long" << "outer_proj_1_long")
                             << BSON_ARRAY("key_string_2_long" << "outer_proj_2_long"))));
    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("key_string_1_long" << "inner_proj_1_long")
                             << BSON_ARRAY("key_string_2_long" << "inner_proj_2_long"))));

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerT, outerV);

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerT, innerV);

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerSlots[0]),
                                                          makeSV(outerSlots[1]),
                                                          makeSV(innerSlots[0]),
                                                          makeSV(innerSlots[1]));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    // Produce one row; the outer subtree now holds views into its current row's buffer.
    ASSERT_EQ(hashJoinStage->getNext(), PlanState::ADVANCED);

    // Mid-stream re-open, as a nested loop join does for its inner side.
    hashJoinStage->open(true /* reOpen */);

    // Simulate the yield check at the top of the re-opened getNext(), before the outer child has
    // produced a fresh row.
    hashJoinStage->saveState();
    hashJoinStage->restoreState();

    // The re-opened join must produce the full result set again.
    std::multiset<std::string> actual;
    while (hashJoinStage->getNext() == PlanState::ADVANCED) {
        auto str = [&](int idx) {
            auto [tag, val] = resultAccessors[idx]->getViewOfValue();
            return std::string(value::getStringView(tag, val));
        };
        actual.insert(str(0) + "|" + str(2) + "|" + str(3));
    }
    std::multiset<std::string> expected{"key_string_1_long|inner_proj_1_long|outer_proj_1_long",
                                        "key_string_2_long|inner_proj_2_long|outer_proj_2_long"};
    ASSERT_TRUE(actual == expected);
    hashJoinStage->close();
}

TEST_F(HashJoinStageTest, NoUseAfterFreeWhenProbeRowIsRecopiedOnSecondYield) {
    auto ctx = makeCompileCtx();

    // One outer row with two build-side matches, so the join publishes the same probe row from two
    // consecutive getNext() calls, with a yield during each.
    value::TagValueOwned outerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("key_string_1_long" << "outer_proj_1_long"))));

    value::TagValueOwned innerArr = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY("key_string_1_long" << "inner_proj_1_long")
                             << BSON_ARRAY("key_string_1_long" << "inner_proj_2_long"))));

    // Turn both sides' strings into heap values (StringBig) with concat(): the kind that a stage
    // holding only a view of them must not copy at yield time.
    auto makeHeapStrings = [&](std::unique_ptr<PlanStage> input, const value::SlotVector& inSlots) {
        SlotExprPairVector projects;
        value::SlotVector outSlots;
        for (auto slot : inSlots) {
            auto [sfxTag, sfxVal] = value::makeNewString("_heap");
            outSlots.push_back(generateSlotId());
            projects.emplace_back(
                outSlots.back(),
                makeE<EFunction>(EFn::kConcat,
                                 makeEs(makeE<EVariable>(slot), makeE<EConstant>(sfxTag, sfxVal))));
        }
        return std::make_pair(
            std::move(outSlots),
            makeS<ProjectStage>(std::move(input), std::move(projects), kEmptyPlanNodeId));
    };

    auto [outerT, outerV] = outerArr.releaseToRaw();
    auto [outerScanSlots, outerScan] = generateVirtualScanMulti(2, outerT, outerV);
    auto [outerSlots, outerStage] = makeHeapStrings(std::move(outerScan), outerScanSlots);

    auto [innerT, innerV] = innerArr.releaseToRaw();
    auto [innerScanSlots, innerScan] = generateVirtualScanMulti(2, innerT, innerV);
    auto [innerSlots, innerStage] = makeHeapStrings(std::move(innerScan), innerScanSlots);

    auto [hjOutputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                            std::move(outerStage),
                                                            std::move(innerStage),
                                                            makeSV(outerSlots[0]),
                                                            makeSV(outerSlots[1]),
                                                            makeSV(innerSlots[0]),
                                                            makeSV(innerSlots[1]));

    // Pass-through projection above the join: its accessor caches a view of the join's output
    // value rather than re-reading the join's accessor on every access.
    auto passThroughSlot = generateSlotId();
    SlotExprPairVector passThrough;
    passThrough.emplace_back(passThroughSlot, makeE<EVariable>(outerSlots[1]));
    auto projectStage =
        makeS<ProjectStage>(std::move(hashJoinStage), std::move(passThrough), kEmptyPlanNodeId);

    // Nested loop inner side: a one-row scan that yields on every getNext(), under a filter that
    // reads the pass-through slot (correlated) after that yield.
    auto [innerXSlot, innerXScan] = generateVirtualScan(BSON_ARRAY(1));
    auto yieldStage = std::make_unique<RecyclingYieldEveryStage>(
        std::move(innerXScan), makeSV(innerXSlot), 1 /* yieldEveryN */, kEmptyPlanNodeId);
    auto* yieldStagePtr = yieldStage.get();
    auto [expectedTag, expectedVal] = value::makeNewString("outer_proj_1_long_heap");
    auto filterStage =
        makeS<FilterStage<false>>(std::move(yieldStage),
                                  makeE<EPrimBinary>(EPrimBinary::eq,
                                                     makeE<EVariable>(passThroughSlot),
                                                     makeE<EConstant>(expectedTag, expectedVal)),
                                  kEmptyPlanNodeId);

    auto loopJoin = makeS<LoopJoinStage>(std::move(projectStage),
                                         std::move(filterStage),
                                         makeSV(passThroughSlot) /* outerProjects */,
                                         makeSV(passThroughSlot) /* outerCorrelated */,
                                         nullptr /* predicate */,
                                         kEmptyPlanNodeId);

    prepareTree(ctx.get(), loopJoin.get());
    yieldStagePtr->setRoot(loopJoin.get());
    auto* resultAccessor = loopJoin->getAccessor(*ctx, passThroughSlot);

    auto assertResult = [&] {
        auto [tag, val] = resultAccessor->getViewOfValue();
        ASSERT_TRUE(value::isString(tag));
        ASSERT_EQ(std::string(value::getStringView(tag, val)), "outer_proj_1_long_heap");
    };

    // Match 1. The inner side yields: the join owns the probe row's values in place; the
    // pass-through slot views them.
    ASSERT_EQ(loopJoin->getNext(), PlanState::ADVANCED);
    assertResult();

    // Match 2. The inner side yields again: the re-save must not move or free the owned values,
    // because the filter reads them right after.
    ASSERT_EQ(loopJoin->getNext(), PlanState::ADVANCED);
    assertResult();

    ASSERT_EQ(loopJoin->getNext(), PlanState::IS_EOF);
    loopJoin->close();
}


}  // namespace mongo::sbe
