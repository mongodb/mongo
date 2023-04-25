/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
 * This file contains a test framework for testing sbe::PlanStages.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"

#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::sbe {

void PlanStageTestFixture::assertValuesEqual(value::TypeTags lhsTag,
                                             value::Value lhsVal,
                                             value::TypeTags rhsTag,
                                             value::Value rhsVal) {
    const auto equal = valueEquals(lhsTag, lhsVal, rhsTag, rhsVal);
    if (!equal) {
        std::stringstream ss;
        ss << std::make_pair(lhsTag, lhsVal) << " != " << std::make_pair(rhsTag, rhsVal);
        LOGV2(5075401, "{msg}", "msg"_attr = ss.str());
    }
    ASSERT_TRUE(equal);
}
std::pair<value::SlotId, std::unique_ptr<PlanStage>> PlanStageTestFixture::generateVirtualScan(
    const BSONArray& array) {
    auto [arrTag, arrVal] = stage_builder::makeValue(array);
    return generateVirtualScan(arrTag, arrVal);
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
    if (!operationContext()->lockState()->isLocked()) {
        globalLock.emplace(operationContext(), MODE_IS);
    }
    if (_yieldPolicy) {
        _yieldPolicy->clearRegisteredPlans();
        _yieldPolicy->registerPlan(root);
    }
    root->attachToOperationContext(operationContext());
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
    auto [resultsTag, resultsVal] = value::makeNewArray();
    value::ValueGuard guard{resultsTag, resultsVal};
    auto resultsView = value::getArrayView(resultsVal);
    // Loop and repeatedly call getNext() until we reach the end, storing the values produced
    // into the array.
    size_t i = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), ++i) {
        auto [tag, val] = accessor->copyOrMoveValue();
        resultsView->push_back(tag, val);

        // Test out saveState() and restoreState() for 50% of the documents (the first document,
        // the third document, the fifth document, and so on).
        if (i % 2 == 0) {
            const bool relinquishCursor = true;
            const bool disableSlotAccess = true;
            stage->saveState(relinquishCursor, disableSlotAccess);
            stage->restoreState(relinquishCursor);
        }
    }

    guard.reset();
    return {resultsTag, resultsVal};
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::getAllResultsMulti(
    PlanStage* stage, std::vector<value::SlotAccessor*> accessors) {
    // Allocate an SBE array to hold the results.
    auto [resultsTag, resultsVal] = value::makeNewArray();
    value::ValueGuard resultsGuard{resultsTag, resultsVal};
    auto resultsView = value::getArrayView(resultsVal);

    // Loop and repeatedly call getNext() until we reach the end.
    size_t j = 0;
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext(), ++j) {
        // Create a new SBE array (`arr`) containing the values produced by each SlotAccessor
        // and insert `arr` into the array of results.
        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard{arrTag, arrVal};
        auto arrView = value::getArrayView(arrVal);
        for (size_t i = 0; i < accessors.size(); ++i) {
            auto [tag, val] = accessors[i]->copyOrMoveValue();
            arrView->push_back(tag, val);
        }
        guard.reset();
        resultsView->push_back(arrTag, arrVal);

        // Test out saveState() and restoreState() for 50% of the documents (the first document,
        // the third document, the fifth document, and so on).
        if (j % 2 == 0) {
            const bool relinquishCursor = true;
            const bool disableSlotAccess = true;
            stage->saveState(relinquishCursor, disableSlotAccess);
            stage->restoreState(relinquishCursor);
        }
    }

    resultsGuard.reset();
    return {resultsTag, resultsVal};
}

void PlanStageTestFixture::runTest(value::TypeTags inputTag,
                                   value::Value inputVal,
                                   value::TypeTags expectedTag,
                                   value::Value expectedVal,
                                   const MakeStageFn<value::SlotId>& makeStage) {
    auto ctx = makeCompileCtx();

    // Set up a ValueGuard to ensure `expected` gets released.
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    // Generate a mock scan from `input` with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Call the `makeStage` callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlot, stage] = makeStage(scanSlot, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), outputSlot);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Compare the results produced with the expected output and assert that they match.
    assertValuesEqual(resultsTag, resultsVal, expectedTag, expectedVal);
}

void PlanStageTestFixture::runTestMulti(int32_t numInputSlots,
                                        value::TypeTags inputTag,
                                        value::Value inputVal,
                                        value::TypeTags expectedTag,
                                        value::Value expectedVal,
                                        const MakeStageFn<value::SlotVector>& makeStageMulti) {
    auto ctx = makeCompileCtx();

    // Set up a ValueGuard to ensure `expected` gets released.
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    // Generate a mock scan from `input` with multiple output slots.
    auto [scanSlots, scanStage] = generateVirtualScanMulti(numInputSlots, inputTag, inputVal);

    // Call the `makeStageMulti` callback to create the PlanStage that we want to test, passing
    // in the mock scan subtree and its output slots.
    auto [outputSlots, stage] = makeStageMulti(scanSlots, std::move(scanStage));

    // Prepare the tree and get the SlotAccessors for the output slots.
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResultsMulti(stage.get(), resultAccessors);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Compare the results produced with the expected output and assert that they match.
    assertValuesEqual(resultsTag, resultsVal, expectedTag, expectedVal);
}

}  // namespace mongo::sbe
