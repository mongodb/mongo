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

#include <string_view>

namespace mongo::sbe {

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::makeValue(const BSONArray& ba) {
    int numBytes = ba.objsize();
    uint8_t* data = new uint8_t[numBytes];
    memcpy(data, reinterpret_cast<const uint8_t*>(ba.objdata()), numBytes);
    return {value::TypeTags::bsonArray, value::bitcastFrom<uint8_t*>(data)};
}

std::pair<value::TypeTags, value::Value> PlanStageTestFixture::makeValue(const BSONObj& bo) {
    int numBytes = bo.objsize();
    uint8_t* data = new uint8_t[numBytes];
    memcpy(data, reinterpret_cast<const uint8_t*>(bo.objdata()), numBytes);
    return {value::TypeTags::bsonObject, value::bitcastFrom<uint8_t*>(data)};
}

std::pair<value::SlotId, std::unique_ptr<PlanStage>> PlanStageTestFixture::generateMockScan(
    value::TypeTags arrTag, value::Value arrVal) {
    // The value passed in must be an array.
    invariant(value::isArray(arrTag));

    // Make an EConstant expression for the array.
    auto arrayExpression = makeE<EConstant>(arrTag, arrVal);

    // Build the unwind/project/limit/coscan subtree.
    auto projectSlot = generateSlotId();
    auto unwindSlot = generateSlotId();
    auto unwind = makeS<UnwindStage>(
        makeProjectStage(
            makeS<LimitSkipStage>(
                makeS<CoScanStage>(kEmptyPlanNodeId), 1, boost::none, kEmptyPlanNodeId),
            kEmptyPlanNodeId,
            projectSlot,
            std::move(arrayExpression)),
        projectSlot,
        unwindSlot,
        generateSlotId(),  // We don't need an index slot but must to provide it.
        false,             // Don't preserve null and empty arrays.
        kEmptyPlanNodeId);

    // Return the UnwindStage and its output slot. The UnwindStage can be used as an input
    // to other PlanStages.
    return {unwindSlot, std::move(unwind)};
}

std::pair<value::SlotVector, std::unique_ptr<PlanStage>>
PlanStageTestFixture::generateMockScanMulti(int32_t numSlots,
                                            value::TypeTags arrTag,
                                            value::Value arrVal) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] = generateMockScan(arrTag, arrVal);

    // Create a ProjectStage that will read the data from `scanStage` and split it up
    // across multiple output slots.
    value::SlotVector projectSlots;
    value::SlotMap<std::unique_ptr<EExpression>> projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        projectSlots.emplace_back(generateSlotId());
        projections.emplace(
            projectSlots.back(),
            makeE<EFunction>("getElement"sv,
                             makeEs(makeE<EVariable>(scanSlot),
                                    makeE<EConstant>(value::TypeTags::NumberInt32,
                                                     value::bitcastFrom<int32_t>(i)))));
    }

    return {std::move(projectSlots),
            makeS<ProjectStage>(std::move(scanStage), std::move(projections), kEmptyPlanNodeId)};
}

std::pair<value::SlotId, std::unique_ptr<PlanStage>> PlanStageTestFixture::generateMockScan(
    const BSONArray& array) {
    auto [arrTag, arrVal] = makeValue(array);
    return generateMockScan(arrTag, arrVal);
}

std::pair<value::SlotVector, std::unique_ptr<PlanStage>>
PlanStageTestFixture::generateMockScanMulti(int32_t numSlots, const BSONArray& array) {
    auto [arrTag, arrVal] = makeValue(array);
    return generateMockScanMulti(numSlots, arrTag, arrVal);
}

void PlanStageTestFixture::prepareTree(PlanStage* root) {
    root->prepare(*_compileCtx);
    root->attachFromOperationContext(opCtx());
    root->open(false);
}

value::SlotAccessor* PlanStageTestFixture::prepareTree(PlanStage* root, value::SlotId slot) {
    prepareTree(root);
    return root->getAccessor(*_compileCtx, slot);
}

std::vector<value::SlotAccessor*> PlanStageTestFixture::prepareTree(PlanStage* root,
                                                                    value::SlotVector slots) {
    std::vector<value::SlotAccessor*> slotAccessors;

    prepareTree(root);
    for (auto slot : slots) {
        slotAccessors.emplace_back(root->getAccessor(*_compileCtx, slot));
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
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext()) {
        auto [tag, val] = accessor->copyOrMoveValue();
        resultsView->push_back(tag, val);
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
    for (auto st = stage->getNext(); st == PlanState::ADVANCED; st = stage->getNext()) {
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
    }

    resultsGuard.reset();
    return {resultsTag, resultsVal};
}

void PlanStageTestFixture::runTest(value::TypeTags inputTag,
                                   value::Value inputVal,
                                   value::TypeTags expectedTag,
                                   value::Value expectedVal,
                                   const MakeStageFn<value::SlotId>& makeStage) {
    // Set up a ValueGuard to ensure `expected` gets released.
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    // Generate a mock scan from `input` with a single output slot.
    auto [scanSlot, scanStage] = generateMockScan(inputTag, inputVal);

    // Call the `makeStage` callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlot, stage] = makeStage(scanSlot, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto resultAccessor = prepareTree(stage.get(), outputSlot);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Compare the results produced with the expected output and assert that they match.
    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

void PlanStageTestFixture::runTestMulti(int32_t numInputSlots,
                                        value::TypeTags inputTag,
                                        value::Value inputVal,
                                        value::TypeTags expectedTag,
                                        value::Value expectedVal,
                                        const MakeStageFn<value::SlotVector>& makeStageMulti) {
    // Set up a ValueGuard to ensure `expected` gets released.
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    // Generate a mock scan from `input` with multiple output slots.
    auto [scanSlots, scanStage] = generateMockScanMulti(numInputSlots, inputTag, inputVal);

    // Call the `makeStageMulti` callback to create the PlanStage that we want to test, passing
    // in the mock scan subtree and its output slots.
    auto [outputSlots, stage] = makeStageMulti(scanSlots, std::move(scanStage));

    // Prepare the tree and get the SlotAccessors for the output slots.
    auto resultAccessors = prepareTree(stage.get(), outputSlots);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResultsMulti(stage.get(), resultAccessors);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Compare the results produced with the expected output and assert that they match.
    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

}  // namespace mongo::sbe
