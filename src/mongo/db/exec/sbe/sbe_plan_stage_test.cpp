// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains a test framework for testing sbe::PlanStages.
 */

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"

#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/virtual_scan.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

std::pair<value::SlotId, std::unique_ptr<PlanStage>> PlanStageTestFixture::generateVirtualScan(
    value::TagValueMaybeOwned arr, PlanNodeId planNodeId /*= kEmptyPlanNodeId*/) {
    invariant(sbe::value::isArray(arr.tag()));

    auto outputSlot = _slotIdGenerator->generate();
    auto virtualScan = sbe::makeS<sbe::VirtualScanStage>(planNodeId, outputSlot, std::move(arr));

    // Return the VirtualScanStage and its output slot.
    return {outputSlot, std::move(virtualScan)};
}

std::pair<value::SlotId, std::unique_ptr<PlanStage>> PlanStageTestFixture::generateVirtualScan(
    const BSONArray& array) {
    auto [arrTag, arrVal] = stage_builder::makeValue(array);
    return generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, arrTag, arrVal));
}

std::pair<value::SlotVector, std::unique_ptr<PlanStage>>
PlanStageTestFixture::generateVirtualScanMulti(int32_t numSlots,
                                               value::TypeTags arrTag,
                                               value::Value arrVal) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, arrTag, arrVal));

    // Create a ProjectStage that will read the data from 'scanStage' and split it up
    // across multiple output slots.
    sbe::value::SlotVector projectSlots;
    sbe::SlotExprPairVector projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        auto indexExpr = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                    sbe::value::bitcastFrom<int32_t>(i));

        projectSlots.emplace_back(_slotIdGenerator->generate());
        projections.emplace_back(
            projectSlots.back(),
            sbe::makeE<sbe::EFunction>(
                EFn::kGetElement,
                sbe::makeEs(sbe::makeE<sbe::EVariable>(scanSlot), std::move(indexExpr))));
    }

    return {std::move(projectSlots),
            sbe::makeS<sbe::ProjectStage>(
                std::move(scanStage), std::move(projections), kEmptyPlanNodeId)};
}

std::pair<value::SlotVector, std::unique_ptr<PlanStage>>
PlanStageTestFixture::generateVirtualScanMulti(int32_t numSlots, value::TagValueOwned arr) {
    auto [arrTag, arrVal] = arr.releaseToRaw();
    return generateVirtualScanMulti(numSlots, arrTag, arrVal);
}

std::pair<value::SlotVector, std::unique_ptr<PlanStage>>
PlanStageTestFixture::generateVirtualScanMulti(int32_t numSlots, const BSONArray& array) {
    auto [arrTag, arrVal] = stage_builder::makeValue(array);
    return generateVirtualScanMulti(numSlots, arrTag, arrVal);
}

void PlanStageTestFixture::prepareTree(CompileCtx* ctx, PlanStage* root) {
    // We want to avoid recursive locking since this results in yield plans that don't yield when
    // they should.
    boost::optional<Lock::GlobalLock> globalLock;
    if (!shard_role_details::getLocker(operationContext())->isLocked()) {
        globalLock.emplace(operationContext(), MODE_IS);
    }
    if (_yieldPolicy) {
        _yieldPolicy->clearRegisteredPlans();
        _yieldPolicy->registerPlan(root);
    }
    root->attachToOperationContext(operationContext());
    if (_mca) {
        ctx->mca = _mca;
    }
    root->prepare(*ctx);
    root->open(false);
}

value::SlotAccessor* PlanStageTestFixture::prepareTree(CompileCtx* ctx,
                                                       PlanStage* root,
                                                       value::SlotId slot) {
    prepareTree(ctx, root);
    return root->getAccessor(*ctx, slot);
}

std::vector<value::SlotAccessor*> PlanStageTestFixture::prepareTree(CompileCtx* ctx,
                                                                    PlanStage* root,
                                                                    value::SlotVector slots) {
    std::vector<value::SlotAccessor*> slotAccessors;

    prepareTree(ctx, root);
    for (auto slot : slots) {
        slotAccessors.emplace_back(root->getAccessor(*ctx, slot));
    }
    return slotAccessors;
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::getAllResults(
    PlanStage* stage, value::SlotAccessor* accessor) {
    // Allocate an array to hold the results.
    value::TagValueOwned resultsOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultsView = value::getArrayView(resultsOwned.value());
    // Loop and repeatedly call getNext() until we reach the end, storing the values produced
    // into the array.
    size_t i = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), ++i) {
        resultsView->push_back(accessor->getCopyOfValue());

        // Test out saveState() and restoreState() for 50% of the documents (the first document,
        // the third document, the fifth document, and so on).
        if (i % 2 == 0) {
            const bool disableSlotAccess = true;
            stage->saveState(disableSlotAccess);
            stage->restoreState();
        }
    }

    return resultsOwned.releaseToRaw();
}

void PlanStageTestFixture::exhaustStage(PlanStage* stage, value::SlotAccessor* accessor) {
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext()) {
    }
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::getAllResultsMulti(
    PlanStage* stage, std::vector<value::SlotAccessor*> accessors, bool forceSpill) {
    // Allocate an SBE array to hold the results.
    value::TagValueOwned resultsOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto resultsView = value::getArrayView(resultsOwned.value());

    // Loop and repeatedly call getNext() until we reach the end.
    size_t j = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), ++j) {
        // Create a new SBE array (`arr`) containing the values produced by each SlotAccessor
        // and insert `arr` into the array of results.
        value::TagValueOwned arrOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
        auto arrView = value::getArrayView(arrOwned.value());
        for (size_t i = 0; i < accessors.size(); ++i) {
            arrView->push_back(accessors[i]->getCopyOfValue());
        }
        resultsView->push_back_raw(arrOwned.releaseToRaw());

        // Test out saveState() and restoreState() for 50% of the documents (the first document,
        // the third document, the fifth document, and so on).
        if (j % 2 == 0) {
            const bool disableSlotAccess = true;
            stage->saveState(disableSlotAccess);
            stage->restoreState();
        }

        if (forceSpill && resultsView->size() % 3 == 0) {
            // check the forceSpill stage
            stage->forceSpill(nullptr /*yieldPolicy*/);
            if (stage->getMemoryTracker()) {
                ASSERT_EQ(stage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);
            }
        }
    }

    return resultsOwned.releaseToRaw();
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::runTest(
    CompileCtx* ctx,
    value::TypeTags inputTag,
    value::Value inputVal,
    const MakeStageFn<value::SlotId>& makeStage) {
    // Generate a mock scan from `input` with a single output slot.
    auto [scanSlot, scanStage] =
        generateVirtualScan(value::TagValueMaybeOwned::fromRaw(true, inputTag, inputVal));

    // Call the `makeStage` callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlot, stage] = makeStage(scanSlot, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto resultAccessor = prepareTree(ctx, stage.get(), outputSlot);

    // Get all the results produced by the PlanStage we want to test.
    return getAllResults(stage.get(), resultAccessor);
}

void PlanStageTestFixture::runTest(value::TypeTags inputTag,
                                   value::Value inputVal,
                                   value::TypeTags expectedTag,
                                   value::Value expectedVal,
                                   const MakeStageFn<value::SlotId>& makeStage) {
    // Set up a TagValueOwned to ensure `expected` gets released.
    value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(expectedTag, expectedVal);

    auto ctx = makeCompileCtx();
    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(runTest(ctx.get(), inputTag, inputVal, makeStage));

    // Compare the results produced with the expected output and assert that they match.
    ASSERT_SBE_VALUE_EQ(resultsOwned.tag(), resultsOwned.value(), expectedTag, expectedVal);
}

void PlanStageTestFixture::runTest(value::TagValueOwned input,
                                   value::TagValueOwned expected,
                                   const MakeStageFn<value::SlotId>& makeStage) {
    auto [inputTag, inputVal] = input.releaseToRaw();
    auto [expectedTag, expectedVal] = expected.releaseToRaw();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStage);
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::runTestMulti(
    size_t numInputSlots,
    value::TypeTags inputTag,
    value::Value inputVal,
    const MakeStageFn<value::SlotVector>& makeStageMulti,
    bool forceSpill,
    const AssertStageStatsFn& assertStageStats) {
    auto ctx = makeCompileCtx();

    // Generate a mock scan from `input` with multiple output slots.
    auto [scanSlots, scanStage] = generateVirtualScanMulti(numInputSlots, inputTag, inputVal);

    // Call the `makeStage` callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlots, stage] = makeStageMulti(scanSlots, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

    // Get all the results produced by the PlanStage we want to test.
    auto results = getAllResultsMulti(stage.get(), resultAccessors, forceSpill);
    if (assertStageStats) {
        assertStageStats(stage->getSpecificStats());
    }
    return results;
}


void PlanStageTestFixture::runTestMulti(int32_t numInputSlots,
                                        value::TypeTags inputTag,
                                        value::Value inputVal,
                                        value::TypeTags expectedTag,
                                        value::Value expectedVal,
                                        const MakeStageFn<value::SlotVector>& makeStageMulti,
                                        bool forceSpill,
                                        const AssertStageStatsFn& assertStageStats) {
    // Set up a TagValueOwned to ensure `expected` gets released.
    value::TagValueOwned expectedOwned = value::TagValueOwned::fromRaw(expectedTag, expectedVal);

    value::TagValueOwned resultsOwned = value::TagValueOwned::fromRaw(runTestMulti(
        numInputSlots, inputTag, inputVal, makeStageMulti, forceSpill, assertStageStats));

    // Compare the results produced with the expected output and assert that they match.
    ASSERT_SBE_VALUE_EQ(resultsOwned.tag(), resultsOwned.value(), expectedTag, expectedVal);
}

}  // namespace mongo::sbe
