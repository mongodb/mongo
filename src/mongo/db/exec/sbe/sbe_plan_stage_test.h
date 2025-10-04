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
 * This file contains a unittest framework for testing sbe::PlanStages.
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/yieldable.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/id_generator.h"

namespace mongo::sbe {

inline auto makeInt32Constant(int32_t num) {
    auto val = sbe::value::bitcastFrom<int32_t>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, val);
}

inline auto makeBoolConstant(bool boolVal) {
    auto val = sbe::value::bitcastFrom<bool>(boolVal);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, val);
}

inline auto makeStringConstant(StringData value) {
    auto [tag, val] = value::makeNewString(value);
    return sbe::makeE<sbe::EConstant>(tag, val);
}

inline auto makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
    return sbe::makeE<sbe::EConstant>(tag, val);
}

inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name,
                                                      sbe::EExpression::Vector args) {
    return sbe::makeE<sbe::EFunction>(name, std::move(args));
}

template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

inline std::unique_ptr<sbe::EExpression> makeVariable(sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(slotId);
}

inline std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId,
                                                      sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId);
}

template <typename T>
using MakeStageFn = std::function<std::pair<T, std::unique_ptr<PlanStage>>(
    T scanSlots, std::unique_ptr<PlanStage> scanStage)>;

using AssertStageStatsFn = std::function<void(const SpecificStats*)>;

template <class... ACCUMULATOR>
requires(std::convertible_to<std::decay_t<ACCUMULATOR>, std::unique_ptr<HashAggAccumulator>> && ...)
inline std::vector<std::unique_ptr<HashAggAccumulator>> makeHashAggAccumulatorList(
    ACCUMULATOR&&... contents) {
    std::vector<std::unique_ptr<HashAggAccumulator>> result;
    (result.emplace_back(std::move(contents)), ...);
    return result;
}

/**
 * PlanStageTestFixture is a unittest framework for testing sbe::PlanStages.
 *
 * To facilitate writing unittests for PlanStages, PlanStageTestFixture sets up an OperationContext
 * and offers a number of methods to help unittest writers. From the perspective a unittest writer,
 * the most important methods in the PlanStageTestFixture class are prepareTree(), runTest(), and
 * runTestMulti(). Each unittest should directly call only one of these methods once.
 *
 * For unittests where you need more control and flexibility, calling prepareTree() directly is
 * the way to go. prepareTree() takes a CompileCtx, the root stage of a PlanStage tree and 0 or more
 * SlotIds as parameters. When invoked, prepareTree() calls prepare() on the root stage (passing in
 * the CompileCtx), attaches the OperationContext to the root stage, calls open() on the root stage,
 * and then returns the SlotAccessors corresponding to the specified SlotIds. For a given unittest
 * that calls prepareTree() directly, you can think of the unittest as having two parts: (1) the
 * part before prepareTree(); and (2) the part after prepareTree(). The first part of the test
 * (before prepareTree()) should do whatever is needed to construct the desired PlanStage tree.
 * The second part of the test (after prepareTree()) should drive the execution of the PlanStage
 * tree (by calling getNext() on the root stage one or more times) and verify that the PlanStage
 * tree behaves as expected. During the first part before prepareTree(), it's common to use
 * generateVirtualScan() or generateVirtualScanMulti() which provide an easy way to build a
 * PlanStage subtree that streams out the contents of an SBE array (mimicking a real collection
 * scan).
 *
 * For unittests where you just need to stream the contents of an input array to a PlanStage and
 * compare the values produced against an "expected output" array, runTest() or runTestMulti() are
 * the way to go. For tests where the PlanStage only has 1 input slot and the test only needs to
 * observe 1 output slot, use runTest(). For unittests where the PlanStage has multiple input slots
 * and/or where the test needs to observe multiple output slots, use runTestMulti().
 */
class PlanStageTestFixture : public CatalogTestFixture {
public:
    PlanStageTestFixture(bool enableYield = true) : _enableYield(enableYield) {};

    void setUp() override {
        CatalogTestFixture::setUp();
        _yieldPolicy = _enableYield ? makeYieldPolicy() : nullptr;
        _slotIdGenerator.reset(new value::SlotIdGenerator());
        _spoolIdGenerator.reset(new value::SpoolIdGenerator());
    }

    void tearDown() override {
        _spoolIdGenerator.reset(nullptr);
        _slotIdGenerator.reset(nullptr);
        _yieldPolicy.reset(nullptr);
        CatalogTestFixture::tearDown();
    }

    value::SlotId generateSlotId() {
        return _slotIdGenerator->generate();
    }

    value::SlotVector generateMultipleSlotIds(int numSlots) {
        return _slotIdGenerator->generateMultiple(numSlots);
    }

    SpoolId generateSpoolId() {
        return _spoolIdGenerator->generate();
    }

    PlanYieldPolicySBE* getYieldPolicy() const {
        return _yieldPolicy.get();
    }

    /**
     * Makes a new CompileCtx suitable for preparing an sbe::PlanStage tree.
     */
    std::unique_ptr<CompileCtx> makeCompileCtx(
        std::unique_ptr<RuntimeEnvironment> env = std::make_unique<RuntimeEnvironment>()) {
        return std::make_unique<CompileCtx>(std::move(env));
    }

    /**
     * Compare two SBE values for equality.
     */
    static bool valueEquals(value::TypeTags lhsTag,
                            value::Value lhsVal,
                            value::TypeTags rhsTag,
                            value::Value rhsVal) {
        auto [cmpTag, cmpVal] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
        return (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);
    }

    /**
     * Asserts the two values are equal. Will write a log message and abort() if they are not.
     */
    static void assertValuesEqual(value::TypeTags lhsTag,
                                  value::Value lhsVal,
                                  value::TypeTags rhsTag,
                                  value::Value rhsVal);

    /**
     * This method takes an SBE array and returns an output slot and a unwind/project/limit/coscan
     * subtree that streams out the elements of the array one at a time via the output slot over a
     * series of calls to getNext(), mimicking the output of a collection scan or an index scan.
     *
     * Note that this method assumes ownership of the SBE Array being passed in.
     */
    std::pair<value::SlotId, std::unique_ptr<PlanStage>> generateVirtualScan(
        value::TypeTags arrTag, value::Value arrVal, PlanNodeId planNodeId = kEmptyPlanNodeId);

    /**
     * This method is similar to generateVirtualScan(), except that the subtree returned outputs to
     * multiple slots instead of a single slot. `numSlots` specifies the number of output slots.
     * `array` is expected to be an array of subarrays. Each subarray is expected to have exactly
     * `numSlots` elements, where the value at index 0 corresponds to output slot 0, the value at
     * index 1 corresponds to output slot 1, and so on. The first subarray supplies the values for
     * the output slots for the first call to getNext(), the second subarray applies the values for
     * the output slots for the second call to getNext(), and so on.
     *
     * Note that this method assumes ownership of the SBE Array being passed in.
     */
    std::pair<value::SlotVector, std::unique_ptr<PlanStage>> generateVirtualScanMulti(
        int32_t numSlots, value::TypeTags arrTag, value::Value arrVal);

    /**
     * Make a mock scan from an BSON array. This method does NOT assume ownership of the BSONArray
     * passed in.
     */
    std::pair<value::SlotId, std::unique_ptr<PlanStage>> generateVirtualScan(
        const BSONArray& array);

    /**
     * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
     * ownership of the BSONArray passed in.
     */
    std::pair<value::SlotVector, std::unique_ptr<PlanStage>> generateVirtualScanMulti(
        int32_t numSlots, const BSONArray& array);

    /**
     * Prepares the tree of PlanStages given by `root`.
     */
    void prepareTree(CompileCtx* ctx, PlanStage* root);

    /**
     * Prepares the tree of PlanStages given by `root` and returns the SlotAccessor* for `slot`.
     */
    value::SlotAccessor* prepareTree(CompileCtx* ctx, PlanStage* root, value::SlotId slot);

    /**
     * Prepares the tree of PlanStages given by `root` and returns the SlotAccessor*'s for
     * the specified slots.
     */
    std::vector<value::SlotAccessor*> prepareTree(CompileCtx* ctx,
                                                  PlanStage* root,
                                                  value::SlotVector slots);

    /**
     * This method repeatedly calls getNext() on the specified PlanStage, stores all the values
     * produced by the specified SlotAccessor into an SBE array, and returns the array.
     *
     * Note that the caller assumes ownership of the SBE array returned.
     */
    std::pair<value::TypeTags, value::Value> getAllResults(PlanStage* stage,
                                                           value::SlotAccessor* accessor);

    /**
     * This method is similar to getAllResults(), except that it supports multiple SlotAccessors.
     * This method returns an array of subarrays. Each subarray contains exactly N elements (where
     * N is the number of output slots) with the value at index 0 corresponding to output slot 0,
     * the value at index 1 corresponding to output slot 1, and so on. The first subarray holds the
     * first values produced by each slot, the second subarray holds the second values produced by
     * each slot, and so on.
     *
     * Note that the caller assumes ownership of the SBE array returned.
     */
    std::pair<value::TypeTags, value::Value> getAllResultsMulti(
        PlanStage* stage, std::vector<value::SlotAccessor*> accessors, bool forceSpill = false);

    /**
     * This method is intended to make it easy to write basic tests. The caller passes in an input
     * array, an array containing the expected output, and a lambda for constructing the PlanStage
     * to be tested. The `makeStage` lambda is passed the input stage and the input slot, and is
     * expected to return a PlanStage and its output slot.
     *
     * This method assumes that the input array should be streamed to the PlanStage via a single
     * slot. Also, for comparing the PlanStage's output to expected output, this method assumes
     * there is only one relevant output slot. For writing basic tests that involve multiple input
     * slots or that involve testing multiple output slots, runTestMulti() should be used instead.
     */
    void runTest(value::TypeTags inputTag,
                 value::Value inputVal,
                 value::TypeTags expectedTag,
                 value::Value expectedVal,
                 const MakeStageFn<value::SlotId>& makeStage);

    // Same method as above, but requires providing your own expression context.
    std::pair<value::TypeTags, value::Value> runTest(CompileCtx* ctx,
                                                     value::TypeTags inputTag,
                                                     value::Value inputVal,
                                                     const MakeStageFn<value::SlotId>& makeStage);

    /**
     * This method is similar to runTest(), but it allows for streaming input via multiple slots as
     * well as testing against multiple output slots. The caller passes in an integer indicating the
     * number of input slots, an input array, an array containing the expected output, and a lambda
     * for constructing the PlanStage to be tested. The `makeStage` lambda is passed the input stage
     * and input slots, and is expected to return a PlanStage and its output slots. `input` should
     * be an array of subarrays with each subarray having N elements, where N is the number of input
     * slots. `output` should be an array of subarrays with each subarray having M elements, where M
     * is the number of output slots.
     */
    void runTestMulti(int32_t numInputSlots,
                      value::TypeTags inputTag,
                      value::Value inputVal,
                      value::TypeTags expectedTag,
                      value::Value expectedVal,
                      const MakeStageFn<value::SlotVector>& makeStageMulti,
                      bool forceSpill = false,
                      const AssertStageStatsFn& assertStageStats = AssertStageStatsFn{});

    // Similar to above method but returns the result instead of comparing to an expected.
    std::pair<value::TypeTags, value::Value> runTestMulti(
        size_t numInputSlots,
        value::TypeTags inputTag,
        value::Value inputVal,
        const MakeStageFn<value::SlotVector>& makeStageMulti,
        bool forceSpill = false,
        const AssertStageStatsFn& assertStageStats = AssertStageStatsFn{});

    value::SlotIdGenerator* getSlotIdGenerator() {
        return _slotIdGenerator.get();
    }

protected:
    std::unique_ptr<PlanYieldPolicySBE> makeYieldPolicy() {
        return PlanYieldPolicySBE::make(
            operationContext(),
            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
            operationContext()->getServiceContext()->getFastClockSource(),
            0,
            Milliseconds::zero());
    }

private:
    class MockYieldable : public Yieldable {
        bool yieldable() const override {
            return true;
        }
        void yield() const override {}
        void restore() const override {}
    };

    MockYieldable _yieldable;
    bool _enableYield;
    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;
    std::unique_ptr<value::SlotIdGenerator> _slotIdGenerator;
    std::unique_ptr<value::SpoolIdGenerator> _spoolIdGenerator;
};

}  // namespace mongo::sbe
