/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/data_type_endian.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/sort_spec.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/column_op.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/code_fragment.h"
#include "mongo/db/exec/sbe/vm/vm_builtin.h"
#include "mongo/db/exec/sbe/vm/vm_memory.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/optional/optional.hpp>

#if !defined(MONGO_CONFIG_DEBUG_BUILD)
#define MONGO_COMPILER_ALWAYS_INLINE_OPT MONGO_COMPILER_ALWAYS_INLINE
#else
#define MONGO_COMPILER_ALWAYS_INLINE_OPT
#endif

namespace mongo {
namespace sbe {
namespace vm {
/**
 * This enum defines indices into an 'Array' that store state for $AccumulatorN expressions.
 *
 * The array contains five elements:
 * - The element at index `kInternalArr` is the array that holds the values.
 * - The element at index `kStartIdx` is the logical start index in the internal array. This is
 *   used for emulating queue behaviour.
 * - The element at index `kMaxSize` is the maximum number entries the data structure holds.
 * - The element at index `kMemUsage` holds the current memory usage
 * - The element at index `kMemLimit` holds the max memory limit allowed
 * - The element at index `kIsGroupAccum` specifies if the accumulator belongs to group-by stage
 */
enum class AggMultiElems {
    kInternalArr,
    kStartIdx,
    kMaxSize,
    kMemUsage,
    kMemLimit,
    kIsGroupAccum,
    kSizeOfArray
};

/**
 * Flags controlling runtime behavior of a magical traverse intrinsic used for evaluating numerical
 * paths.
 * If kPreTraverse is specified then we run the traverse before calling getField/getElement.
 * If kPostTraverse is specified then we run the traverse after calling getField/getElement.
 * Note that we can freely combine pre and post flags; i.e. they are not mutually exclusive.
 */
enum MagicTraverse : int32_t { kPreTraverse = 1, kPostTraverse = 2 };

/**
 * Less than comparison based on a sort pattern.
 */
struct SortPatternLess {
    SortPatternLess(const SortSpec* sortSpec) : _sortSpec(sortSpec) {}

    bool operator()(const std::pair<value::TypeTags, value::Value>& lhs,
                    const std::pair<value::TypeTags, value::Value>& rhs) const {
        auto [cmpTag, cmpVal] = _sortSpec->compare(lhs.first, lhs.second, rhs.first, rhs.second);
        uassert(5807000, "Invalid comparison result", cmpTag == value::TypeTags::NumberInt32);
        return value::bitcastTo<int32_t>(cmpVal) < 0;
    }

private:
    const SortSpec* _sortSpec;
};

/**
 * Greater than comparison based on a sort pattern.
 */
struct SortPatternGreater {
    SortPatternGreater(const SortSpec* sortSpec) : _sortSpec(sortSpec) {}

    bool operator()(const std::pair<value::TypeTags, value::Value>& lhs,
                    const std::pair<value::TypeTags, value::Value>& rhs) const {
        auto [cmpTag, cmpVal] = _sortSpec->compare(lhs.first, lhs.second, rhs.first, rhs.second);
        uassert(5807001, "Invalid comparison result", cmpTag == value::TypeTags::NumberInt32);
        return value::bitcastTo<int32_t>(cmpVal) > 0;
    }

private:
    const SortSpec* _sortSpec;
};

/**
 * Comparison based on the key of a pair of elements.
 */
template <typename Comp>
struct PairKeyComp {
    PairKeyComp(const Comp& comp) : _comp(comp) {}

    bool operator()(const std::pair<value::TypeTags, value::Value>& lhs,
                    const std::pair<value::TypeTags, value::Value>& rhs) const {
        auto [lPairTag, lPairVal] = lhs;
        auto lPair = value::getArrayView(lPairVal);
        auto lKey = lPair->getAt(0);

        auto [rPairTag, rPairVal] = rhs;
        auto rPair = value::getArrayView(rPairVal);
        auto rKey = rPair->getAt(0);

        return _comp(lKey, rKey);
    }

private:
    const Comp _comp;
};

struct GetSortKeyAscFunctor {
    GetSortKeyAscFunctor(CollatorInterface* collator = nullptr) : collator(collator) {}
    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const;
    CollatorInterface* collator = nullptr;
};

struct GetSortKeyDescFunctor {
    GetSortKeyDescFunctor(CollatorInterface* collator = nullptr) : collator(collator) {}
    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const;
    CollatorInterface* collator = nullptr;
};

extern const value::ColumnOpInstanceWithParams<value::ColumnOpType::kNoFlags, GetSortKeyAscFunctor>
    getSortKeyAscOp;

extern const value::ColumnOpInstanceWithParams<value::ColumnOpType::kNoFlags, GetSortKeyDescFunctor>
    getSortKeyDescOp;

int32_t updateAndCheckMemUsage(value::Array* state,
                               int32_t memUsage,
                               int32_t memAdded,
                               int32_t memLimit,
                               size_t idx = static_cast<size_t>(AggMultiElems::kMemUsage));

class CodeFragment;

/**
 * This enum defines indices into an 'Array' that returns the partial sum result when 'needsMerge'
 * is requested.
 *
 * See 'builtinDoubleDoubleSumFinalize()' for more details.
 */
enum class AggPartialSumElems { kTotal, kError, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that accumulates $stdDevPop and $stdDevSamp results.
 *
 * The array contains 3 elements:
 * - The element at index `kCount` keeps track of the total number of values processed
 * - The elements at index `kRunningMean` keeps track of the mean of all the values that have been
 * processed.
 * - The elements at index `kRunningM2` keeps track of running M2 value (defined within:
 * https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm)
 * for all the values that have been processed.
 *
 * See 'aggStdDevImpl()'/'aggStdDev()'/'stdDevPopFinalize() / stdDevSampFinalize()' for more
 * details.
 */
enum AggStdDevValueElems {
    kCount,
    kRunningMean,
    kRunningM2,
    // This is actually not an index but represents the number of elements stored
    kSizeOfArray
};

/**
 * This enum defines indices into an 'Array' that store state for rank expressions.
 *
 * The array contains three elements:
 * - The element at index `kLastValue` is the last value.
 * - The element at index `kLastValueIsNothing` is true if the last value is nothing.
 * - The element at index `kLastRank` is the rank of the last value.
 * - The element at index `kSameRankCount` is how many values are of the same rank as the last
 * value.
 * - The element at index `kSortSpec` is the sort spec object used to generate sort key for adjacent
 * value comparison.
 */
enum AggRankElems {
    kLastValue,
    kLastValueIsNothing,
    kLastRank,
    kSameRankCount,
    kSortSpec,
    kRankArraySize
};

/**
 * This enum defines indices into an 'Array' that returns the result of accumulators that track the
 * size of accumulated values, such as 'addToArrayCapped' and 'addToSetCapped'.
 */
enum class AggArrayWithSize { kValues = 0, kSizeOfValues, kLast = kSizeOfValues + 1 };

/**
 * This enum defines indices into an 'Array' that stores the state for $expMovingAvg accumulator
 */
enum class AggExpMovingAvgElems { kResult, kAlpha, kIsDecimal, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that stores the state for $sum window function
 * accumulator. Index `kSumAcc` stores the accumulator state of aggDoubleDoubleSum. Rest of the
 * indices store respective count of values encountered.
 */
enum class AggRemovableSumElems {
    kSumAcc,
    kNanCount,
    kPosInfinityCount,
    kNegInfinityCount,
    kDoubleCount,
    kDecimalCount,
    kSizeOfArray
};

/**
 * This enum defines indices into an 'Array' that stores the state for $integral accumulator
 * Element at `kInputQueue` stores the queue of input values
 * Element at `kSortByQueue` stores the queue of sortBy values
 * Element at `kIntegral` stores the integral over the current window
 * Element at `kNanCount` stores the count of NaN values encountered
 * Element at `kunitMillis` stores the date unit (Null if not valid)
 * Element at `kIsNonRemovable` stores whether it belongs to a non-removable window
 */
enum class AggIntegralElems {
    kInputQueue,
    kSortByQueue,
    kIntegral,
    kNanCount,
    kUnitMillis,
    kIsNonRemovable,
    kMaxSizeOfArray
};

/**
 * This enum defines indices into an 'Array' that stores the state for $derivative accumulator
 * Element at `kInputQueue` stores the queue of input values
 * Element at `kSortByQueue` stores the queue of sortBy values
 * Element at `kunitMillis` stores the date unit (Null if not valid)
 */
enum class AggDerivativeElems { kInputQueue, kSortByQueue, kUnitMillis, kMaxSizeOfArray };

/**
 * This enum defines indices into an 'Array' that stores the state for a queue backed by a
 * circular array
 * Element at `kArray` stores the underlying array thats holds the elements. This should be
 * initialized to a non-zero size initially.
 * Element at `kStartIdx` stores the start position of the queue
 * Element at `kQueueSize` stores the size of the queue
 * The empty values in the array are filled with Null
 */
enum class ArrayQueueElems { kArray, kStartIdx, kQueueSize, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that store state for the removable
 * $covarianceSamp/$covariancePop expressions.
 */
enum class AggCovarianceElems { kSumX, kSumY, kCXY, kCount, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that store state for the removable
 * $stdDevSamp/$stdDevPop expressions.
 */
enum class AggRemovableStdDevElems { kSum, kM2, kCount, kNonFiniteCount, kSizeOfArray };

/**
 * This enum defines indices into an `Array` that store state for $linearFill
 * X, Y refers to sortby field and input field respectively
 * At any time, (X1, Y1) and (X2, Y2) defines two end-points with non-null input values
 * with zero or more null input values in between. Count stores the number of values left
 * till (X2, Y2). Initially it is equal to number of values between (X1, Y1) and (X2, Y2),
 * exclusive of first and inclusive of latter. It is decremented after each finalize call,
 * till this segment is exhausted and after which we find next segement(new (X2, Y2)
 * while (X1, Y1) is set to previous (X2, Y2))
 */
enum class AggLinearFillElems { kX1, kY1, kX2, kY2, kPrevX, kCount, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that store state for $firstN/$lastN
 * window functions
 */
enum class AggFirstLastNElems { kQueue, kN, kSizeOfArray };

/**
 * This enum defines indices into an 'Array' that store state for $minN/$maxN/$topN/$bottomN
 * window functions.
 * Element at `kValues` stores the accmulator data structure with the elements
 * Element at `kN` stores an integer with the number of values minN/maxN should return
 * Element at `kMemUsage`stores the size of the multiset in bytes
 * Element at `kMemLimit`stores the maximum allowed size of the multiset in bytes
 */
enum class AggAccumulatorNElems { kValues = 0, kN, kMemUsage, kMemLimit, kSizeOfArray };

/**
 * Enum to specify whether to extract top or bottom N elements.
 */
enum class TopBottomSense { kTop, kBottom };

class ByteCode {
    // The number of bytes per stack entry.
    static constexpr size_t sizeOfElement =
        sizeof(bool) + sizeof(value::TypeTags) + sizeof(value::Value);
    static_assert(sizeOfElement == 10);
    static_assert(std::is_trivially_copyable_v<FastTuple<bool, value::TypeTags, value::Value>>);

public:
    class MakeObjImplBase;
    struct InvokeLambdaFunctor;
    class TopBottomArgs;
    class TopBottomArgsDirect;
    class TopBottomArgsFromStack;
    class TopBottomArgsFromBlocks;

    static void aggDoubleDoubleSumImpl(value::Array* accumulator,
                                       value::TypeTags rhsTag,
                                       value::Value rhsValue);
    static value::TagValueMaybeOwned aggDoubleDoubleSumFinalizeImpl(value::Array* accumulator);
    static void aggMergeDoubleDoubleSumsImpl(value::Array* accumulator,
                                             value::TypeTags rhsTag,
                                             value::Value rhsValue);
    value::TagValueMaybeOwned builtinConvertSimpleSumToDoubleDoubleSumImpl(
        value::TypeTags simpleSumTag, value::Value simpleSumVal);
    static FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoublePartialSumFinalizeImpl(
        value::TypeTags fieldTag, value::Value fieldValue);
    static value::TagValueMaybeOwned genericDiv(value::TypeTags lhsTag,
                                                value::Value lhsValue,
                                                value::TypeTags rhsTag,
                                                value::Value rhsValue);

    static value::TagValueMaybeOwned builtinAddToArrayCappedImpl(
        value::TagValueOwned accumulatorState, value::TagValueMaybeOwned newElem, int32_t sizeCap);

    static value::TagValueMaybeOwned addToSetCappedImpl(value::TagValueOwned accumulatorState,
                                                        value::TagValueMaybeOwned newElem,
                                                        int32_t sizeCap,
                                                        CollatorInterface* collator);

    /**
     * Moves the elements from the newArrayElements array onto the end of
     * the accumulatorState capped array accumulator, enforcing the cap by
     * throwing an exception if the operation would result in an accumulator state that exceeds it.
     *
     * Either the accumulator state or new members array may be Nothing, which gets treated as an
     * empty array.
     *
     * The capped accumulator state is a two-element array, where the first element is the array
     * value and the second element is the array value's pre-computed size. The return value is also
     * an array with the same structure.
     */
    static value::TagValueMaybeOwned concatArraysAccumImpl(value::TagValueOwned accumulatorState,
                                                           value::TagValueOwned newArrayElements,
                                                           int64_t newArrayElementsSize,
                                                           int32_t sizeCap);

    /**
     * Moves the elements from the newSetMembers array into the
     * accumulatorState capped set accumulator, preserving set semantics
     * by ignoring duplicates and enforcing the cap by throwing an exception if the operation would
     * result in an accumulator state that exceeds it.
     *
     * Either the accumulator state or new members array may be Nothing, which gets treated as an
     * empty accumlator or empty array, respectively.
     *
     * The capped accumulator state is a two-element array, where the first element is an 'ArraySet'
     * and the second element is the set's pre-computed size. The return value is also an array
     * with the same structure.
     */
    static value::TagValueMaybeOwned setUnionAccumImpl(value::TagValueOwned accumulatorState,
                                                       value::TagValueOwned newSetMembers,
                                                       int32_t sizeCap,
                                                       CollatorInterface* collator);

    ByteCode() {
        _argStack = static_cast<uint8_t*>(::operator new(sizeOfElement * 4));
        _argStackEnd = _argStack + sizeOfElement * 4;
        _argStackTop = _argStack - sizeOfElement;
    }

    ~ByteCode() {
        ::operator delete(_argStack, _argStackEnd - _argStack);
    }

    ByteCode(const ByteCode&) = delete;
    ByteCode& operator=(const ByteCode&) = delete;

    static std::pair<value::TypeTags, value::Value> genericInitializeDoubleDoubleSumState();
    static std::tuple<value::Array*, int64_t, int64_t, int64_t, int64_t, int64_t>
    genericRemovableSumState(value::Array* state);
    static void genericResetDoubleDoubleSumState(value::Array* state);

    /**
     * Runs a CodeFragment representing an arbitrary VM program that returns one slot value.
     */
    value::TagValueMaybeOwned run(const CodeFragment* code);

    /**
     * Runs a CodeFragment reprensenting a predicate that returns a boolean result.
     */
    bool runPredicate(const CodeFragment* code);

    typedef std::tuple<value::Array*, value::Array*, size_t, size_t, int32_t, int32_t, bool>
        MultiAccState;

    FastTuple<bool, value::TypeTags, value::Value> getField_test(value::TypeTags objTag,
                                                                 value::Value objValue,
                                                                 StringData fieldStr) {
        return getField(objTag, objValue, fieldStr);
    }

private:
    /**
     * Executes the VM instructions of an arbitrary CodeFragment starting at 'position'.
     */
    void runInternal(const CodeFragment* code, int64_t position);

    /**
     * Executes the VM instructions of a CodeFragment starting at 'position' that represents a
     * lambda function and leaves its result on the top of the stack.
     */
    void runLambdaInternal(const CodeFragment* code, int64_t position);

    MONGO_COMPILER_NORETURN void runFailInstruction();

    /**
     * Run a usually Boolean check against the tag of the item on top of the stack and add its
     * result to the stack as a TypeTags::Boolean value. However, if the stack item to be checked
     * itself has a tag of TagTypes::Nothing, this instead pushes a result of TagTypes::Nothing.
     */
    template <typename T>
    void runTagCheck(const uint8_t*& pcPointer, T&& predicate);
    void runTagCheck(const uint8_t*& pcPointer, value::TypeTags tagRhs);

    value::TagValueMaybeOwned genericIDiv(value::TypeTags lhsTag,
                                          value::Value lhsValue,
                                          value::TypeTags rhsTag,
                                          value::Value rhsValue);
    value::TagValueMaybeOwned genericMod(value::TypeTags lhsTag,
                                         value::Value lhsValue,
                                         value::TypeTags rhsTag,
                                         value::Value rhsValue);
    value::TagValueMaybeOwned genericAbs(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericCeil(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericFloor(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericExp(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericLn(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericLog10(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericSqrt(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericPow(value::TypeTags baseTag,
                                         value::Value baseValue,
                                         value::TypeTags exponentTag,
                                         value::Value exponentValue);
    value::TagValueMaybeOwned genericRoundTrunc(std::string funcName,
                                                Decimal128::RoundingMode roundingMode,
                                                int32_t place,
                                                value::TypeTags numTag,
                                                value::Value numVal);
    value::TagValueMaybeOwned scalarRoundTrunc(std::string funcName,
                                               Decimal128::RoundingMode roundingMode,
                                               ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> blockRoundTrunc(
        std::string funcName, Decimal128::RoundingMode roundingMode, ArityType arity);
    value::TagValueOwned genericNot(value::TypeTags tag, value::Value value);

    FastTuple<bool, value::TypeTags, value::Value> getField(value::TypeTags objTag,
                                                            value::Value objValue,
                                                            value::TypeTags fieldTag,
                                                            value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> getField(value::TypeTags objTag,
                                                            value::Value objValue,
                                                            StringData fieldStr);

    FastTuple<bool, value::TypeTags, value::Value> getElement(value::TypeTags objTag,
                                                              value::Value objValue,
                                                              value::TypeTags fieldTag,
                                                              value::Value fieldValue);
    FastTuple<bool, value::TypeTags, value::Value> getFieldOrElement(value::TypeTags objTag,
                                                                     value::Value objValue,
                                                                     value::TypeTags fieldTag,
                                                                     value::Value fieldValue);

    void traverseP(const CodeFragment* code);
    void traverseP(const CodeFragment* code,
                   int64_t position,
                   bool providePosition,
                   int64_t maxDepth);
    void traverseP_nested(const CodeFragment* code,
                          int64_t position,
                          value::TypeTags tag,
                          value::Value val,
                          bool providePosition,
                          int64_t maxDepth,
                          int64_t curDepth);

    void traverseF(const CodeFragment* code);
    void traverseF(const CodeFragment* code,
                   int64_t position,
                   bool providePosition,
                   bool compareArray);
    void traverseFInArray(const CodeFragment* code,
                          int64_t position,
                          bool providePosition,
                          bool compareArray);
    void magicTraverseF(const CodeFragment* code);

    bool runLambdaPredicate(const CodeFragment* code, int64_t position);
    void valueBlockApplyLambda(const CodeFragment* code);

    FastTuple<bool, value::TypeTags, value::Value> setField();

    int32_t convertNumericToInt32(value::TypeTags tag, value::Value val);

    FastTuple<bool, value::TypeTags, value::Value> getArraySize(value::TypeTags tag,
                                                                value::Value val);

    FastTuple<bool, value::TypeTags, value::Value> aggSum(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> aggCount(value::TypeTags accTag,
                                                            value::Value accValue);

    // This is an implementation of the following algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    void aggStdDevImpl(value::Array* accumulator, value::TypeTags rhsTag, value::Value rhsValue);
    void aggMergeStdDevsImpl(value::Array* accumulator,
                             value::TypeTags rhsTag,
                             value::Value rhsValue);

    value::TagValueMaybeOwned aggStdDevFinalizeImpl(value::Value fieldValue, bool isSamp);

    FastTuple<bool, value::TypeTags, value::Value> aggMin(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue,
                                                          CollatorInterface* collator = nullptr);

    FastTuple<bool, value::TypeTags, value::Value> aggMax(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue,
                                                          CollatorInterface* collator = nullptr);

    FastTuple<bool, value::TypeTags, value::Value> aggFirst(value::TypeTags accTag,
                                                            value::Value accValue,
                                                            value::TypeTags fieldTag,
                                                            value::Value fieldValue);

    FastTuple<bool, value::TypeTags, value::Value> aggLast(value::TypeTags accTag,
                                                           value::Value accValue,
                                                           value::TypeTags fieldTag,
                                                           value::Value fieldValue);

    value::TagValueMaybeOwned genericAcos(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAcosh(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAsin(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAsinh(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAtan(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAtanh(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericAtan2(value::TypeTags operandTag1,
                                           value::Value operandValue1,
                                           value::TypeTags operandTag2,
                                           value::Value operandValue2);
    value::TagValueMaybeOwned genericCos(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericCosh(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericDegreesToRadians(value::TypeTags operandTag,
                                                      value::Value operandValue);
    value::TagValueMaybeOwned genericRadiansToDegrees(value::TypeTags operandTag,
                                                      value::Value operandValue);
    value::TagValueMaybeOwned genericSin(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericSinh(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericTan(value::TypeTags operandTag, value::Value operandValue);
    value::TagValueMaybeOwned genericTanh(value::TypeTags operandTag, value::Value operandValue);

    FastTuple<bool, value::TypeTags, value::Value> genericDayOfYear(value::TypeTags timezoneDBTag,
                                                                    value::Value timezoneDBValue,
                                                                    value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfYear(value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfMonth(value::TypeTags timezoneDBTag,
                                                                     value::Value timezoneDBValue,
                                                                     value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfMonth(value::TypeTags dateTag,
                                                                     value::Value dateValue,
                                                                     value::TypeTags timezoneTag,
                                                                     value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfWeek(value::TypeTags timezoneDBTag,
                                                                    value::Value timezoneDBValue,
                                                                    value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDayOfWeek(value::TypeTags dateTag,
                                                                    value::Value dateValue,
                                                                    value::TypeTags timezoneTag,
                                                                    value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericYear(value::TypeTags timezoneDBTag,
                                                               value::Value timezoneDBValue,
                                                               value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericYear(value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMonth(value::TypeTags timezoneDBTag,
                                                                value::Value timezoneDBValue,
                                                                value::TypeTags dateTag,
                                                                value::Value dateValue,
                                                                value::TypeTags timezoneTag,
                                                                value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMonth(value::TypeTags dateTag,
                                                                value::Value dateValue,
                                                                value::TypeTags timezoneTag,
                                                                value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericHour(value::TypeTags timezoneDBTag,
                                                               value::Value timezoneDBValue,
                                                               value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericHour(value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMinute(value::TypeTags timezoneDBTag,
                                                                 value::Value timezoneDBValue,
                                                                 value::TypeTags dateTag,
                                                                 value::Value dateValue,
                                                                 value::TypeTags timezoneTag,
                                                                 value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMinute(value::TypeTags dateTag,
                                                                 value::Value dateValue,
                                                                 value::TypeTags timezoneTag,
                                                                 value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSecond(value::TypeTags timezoneDBTag,
                                                                 value::Value timezoneDBValue,
                                                                 value::TypeTags dateTag,
                                                                 value::Value dateValue,
                                                                 value::TypeTags timezoneTag,
                                                                 value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSecond(value::TypeTags dateTag,
                                                                 value::Value dateValue,
                                                                 value::TypeTags timezoneTag,
                                                                 value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMillisecond(value::TypeTags timezoneDBTag,
                                                                      value::Value timezoneDBValue,
                                                                      value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMillisecond(value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericWeek(value::TypeTags timezoneDBTag,
                                                               value::Value timezoneDBValue,
                                                               value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericWeek(value::TypeTags dateTag,
                                                               value::Value dateValue,
                                                               value::TypeTags timezoneTag,
                                                               value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISOWeekYear(value::TypeTags timezoneDBTag,
                                                                      value::Value timezoneDBValue,
                                                                      value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISOWeekYear(value::TypeTags dateTag,
                                                                      value::Value dateValue,
                                                                      value::TypeTags timezoneTag,
                                                                      value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISODayOfWeek(
        value::TypeTags timezoneDBTag,
        value::Value timezoneDBValue,
        value::TypeTags dateTag,
        value::Value dateValue,
        value::TypeTags timezoneTag,
        value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISODayOfWeek(value::TypeTags dateTag,
                                                                       value::Value dateValue,
                                                                       value::TypeTags timezoneTag,
                                                                       value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISOWeek(value::TypeTags timezoneDBTag,
                                                                  value::Value timezoneDBValue,
                                                                  value::TypeTags dateTag,
                                                                  value::Value dateValue,
                                                                  value::TypeTags timezoneTag,
                                                                  value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericISOWeek(value::TypeTags dateTag,
                                                                  value::Value dateValue,
                                                                  value::TypeTags timezoneTag,
                                                                  value::Value timezoneValue);
    FastTuple<bool, value::TypeTags, value::Value> genericNewKeyString(
        ArityType arity, CollatorInterface* collator = nullptr);
    FastTuple<bool, value::TypeTags, value::Value> dateTrunc(value::TypeTags dateTag,
                                                             value::Value dateValue,
                                                             TimeUnit unit,
                                                             int64_t binSize,
                                                             TimeZone timezone,
                                                             DayOfWeek startOfWeek);

    template <bool IsBlockBuiltin = false>
    bool validateDateTruncParameters(TimeUnit* unit,
                                     int64_t* binSize,
                                     TimeZone* timezone,
                                     DayOfWeek* startOfWeek);

    template <bool IsBlockBuiltin = false>
    bool validateDateDiffParameters(Date_t* endDate,
                                    TimeUnit* unit,
                                    TimeZone* timezone,
                                    DayOfWeek* startOfWeek);

    template <bool IsBlockBuiltin = false>
    bool validateDateAddParameters(TimeUnit* unit, int64_t* amount, TimeZone* timezone);

    /**
     * These functions contain the C++ code implementing the matching builtin function that is
     * referenced in the function name (e.g. builtinSplit is invoked when Builtin::split is
     * encountered).
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinSplit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDate(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateWeekYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateDiff(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateToParts(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsoDateToParts(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfMonth(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDayOfWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinKeepFields(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinReplaceOne(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDropFields(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewArrayFromRange(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewObj(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewBsonObj(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinKeyStringToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinNewKeyString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollNewKeyString(ArityType arity);
    value::TagValueMaybeOwned builtinAbs(ArityType arity);
    value::TagValueMaybeOwned builtinCeil(ArityType arity);
    value::TagValueMaybeOwned builtinFloor(ArityType arity);
    value::TagValueMaybeOwned builtinTrunc(ArityType arity);
    value::TagValueMaybeOwned builtinExp(ArityType arity);
    value::TagValueMaybeOwned builtinLn(ArityType arity);
    value::TagValueMaybeOwned builtinLog10(ArityType arity);
    value::TagValueMaybeOwned builtinSqrt(ArityType arity);
    value::TagValueMaybeOwned builtinPow(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArrayCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMergeObjects(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> isMemberImpl(value::TypeTags exprTag,
                                                                value::Value exprVal,
                                                                value::TypeTags arrTag,
                                                                value::Value arrVal,
                                                                CollatorInterface* collator);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetToArray(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinSetUnionCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetUnionCapped(ArityType arity);

    /**
     * If the BSON type of the value at stack[0] matches the BSON type mask at stack[1] (see
     * value::getBSONTypeMask()), returns true, else returns false. (Returns Nothing if stack[0] is
     * Nothing or stack[1] is not a NumberInt32.)
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinTypeMatch(ArityType arity);

    /**
     * If the BSON type of the value at stack[0] matches the BSON type mask at stack[1] (see
     * value::getBSONTypeMask()), returns stack[2] (the fill value), else returns stack[0] (the
     * original value). (Returns Nothing if stack[0] is Nothing or stack[1] is not a NumberInt32.)
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinFillType(ArityType arity);

    value::TagValueMaybeOwned builtinConvertSimpleSumToDoubleDoubleSum(ArityType arity);
    value::TagValueMaybeOwned builtinDoubleDoubleSum(ArityType arity);
    // The template parameter is false for a regular DoubleDouble summation and true if merging
    // partially computed DoubleDouble sums.
    template <bool merging>
    value::TagValueMaybeOwned builtinAggDoubleDoubleSum(ArityType arity);

    value::TagValueMaybeOwned builtinDoubleDoubleSumFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoublePartialSumFinalize(
        ArityType arity);

    // The template parameter is false for a regular std dev and true if merging partially computed
    // standard devations.
    template <bool merging>
    value::TagValueMaybeOwned builtinAggStdDev(ArityType arity);

    value::TagValueMaybeOwned builtinStdDevPopFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinStdDevSampFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestZero(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestMask(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestPosition(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBsonSize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStrLenBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStrLenCP(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSubstrBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSubstrCP(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToUpper(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToLower(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCoerceToBool(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCoerceToString(ArityType arity);
    value::TagValueMaybeOwned builtinAcos(ArityType arity);
    value::TagValueMaybeOwned builtinAcosh(ArityType arity);
    value::TagValueMaybeOwned builtinAsin(ArityType arity);
    value::TagValueMaybeOwned builtinAsinh(ArityType arity);
    value::TagValueMaybeOwned builtinAtan(ArityType arity);
    value::TagValueMaybeOwned builtinAtanh(ArityType arity);
    value::TagValueMaybeOwned builtinAtan2(ArityType arity);
    value::TagValueMaybeOwned builtinCos(ArityType arity);
    value::TagValueMaybeOwned builtinCosh(ArityType arity);
    value::TagValueMaybeOwned builtinDegreesToRadians(ArityType arity);
    value::TagValueMaybeOwned builtinRadiansToDegrees(ArityType arity);
    value::TagValueMaybeOwned builtinSin(ArityType arity);
    value::TagValueMaybeOwned builtinSinh(ArityType arity);
    value::TagValueMaybeOwned builtinTan(ArityType arity);
    value::TagValueMaybeOwned builtinTanh(ArityType arity);
    value::TagValueMaybeOwned builtinRand(ArityType arity);
    value::TagValueMaybeOwned builtinRound(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcat(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcatArrays(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinZipArrays(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTrim(ArityType arity,
                                                               bool trimLeft,
                                                               bool trimRight);
    value::TagValueMaybeOwned builtinAggConcatArraysCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcatArraysCapped(ArityType arity);
    value::TagValueMaybeOwned builtinAggSetUnion(ArityType arity);
    value::TagValueMaybeOwned builtinAggCollSetUnion(ArityType arity);
    value::TagValueMaybeOwned builtinAggSetUnionCapped(ArityType arity);
    value::TagValueMaybeOwned builtinAggCollSetUnionCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsMember(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollIsMember(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIndexOfBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIndexOfCP(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsDayOfWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsTimeUnit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsTimezone(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsValidToStringFormat(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValidateFromStringFormat(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetIntersection(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetDifference(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetEquals(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetIsSubset(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetIntersection(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetDifference(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetEquals(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollSetIsSubset(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRunJsPredicate(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexCompile(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexFind(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRegexFindAll(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinShardFilter(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinShardHash(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinExtractSubArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsArrayEmpty(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinReverseArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSortArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTopN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTop(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBottomN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBottom(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> topOrBottomImpl(ArityType arity,
                                                                   TopBottomSense sense);
    FastTuple<bool, value::TypeTags, value::Value> topOrBottomNImpl(ArityType arity,
                                                                    TopBottomSense sense);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinHasNullBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGetRegexPattern(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGetRegexFlags(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinHash(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinFtsMatch(ArityType arity);
    std::pair<SortSpec*, CollatorInterface*> generateSortKeyHelper(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGenerateSortKey(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinGenerateCheapSortKey(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSortKeyComponentVectorGetElement(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSortKeyComponentVectorToArray(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMakeObj(ArityType arity,
                                                                  const CodeFragment* code);
    FastTuple<bool, value::TypeTags, value::Value> builtinMakeBsonObj(ArityType arity,
                                                                      const CodeFragment* code);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsSecond(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsIncrement(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateFromString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateFromStringNoThrow(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateTrunc(ArityType arity);
    template <bool IsAscending, bool IsLeaf>
    FastTuple<bool, value::TypeTags, value::Value> builtinGetSortKey(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMonth(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinHour(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMinute(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSecond(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMillisecond(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinISOWeekYear(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinISODayOfWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinISOWeek(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinObjectToArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinArrayToObject(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAvgOfArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMaxOfArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMinOfArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> maxMinArrayHelper(ArityType arity, bool isMax);
    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevPop(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevSamp(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> stdDevHelper(ArityType arity, bool isSamp);
    FastTuple<bool, value::TypeTags, value::Value> builtinSumOfArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> avgOrSumOfArrayHelper(ArityType arity,
                                                                         bool isAvg);

    /**
     * Implementation of the builtin function 'unwindArray'. It accepts 1 argument that must be one
     * of the SBE array types (BSONArray, Array, ArraySet, ArrayMultiSet) and returns an Array
     * object that contains all the non-array items of the input, plus the items found in all the
     * array items.
     * E.g. unwindArray([ 1, ['a', ['b']], 2, [] ]) = [ 1, 'a', ['b'], 2 ]
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinUnwindArray(ArityType arity);
    /**
     * Implementation of the builtin function 'arrayToSet'. It accepts 1 argument that must be one
     * of the SBE array types (BSONArray, Array, ArraySet, ArrayMultiSet) and returns an ArraySet
     * object that contains all the non-duplicate items of the input.
     * E.g. arrayToSet([ 1, ['a', ['b']], 2, 1]) = [ 1, ['a', ['b']], 2 ]
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinArrayToSet(ArityType arity);
    /**
     * Implementation of the builtin function 'collArrayToSet'. It accepts 2 arguments; the first
     * one is the collator object to be used when performing comparisons, the second must be one of
     * the SBE array types (BSONArray, Array, ArraySet, ArrayMultiSet). It returns an ArraySet
     * object that contains all the non-duplicate items of the input.
     * E.g. collArrayToSet(<case-insensitive collator>, ['a', ['a'], 'A']) = ['a', ['a']]
     */
    FastTuple<bool, value::TypeTags, value::Value> builtinCollArrayToSet(ArityType arity);

    static MultiAccState getMultiAccState(value::TypeTags stateTag, value::Value stateVal);

    value::TagValueMaybeOwned builtinAggFirstNNeedsMoreInput(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstN(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstNMerge(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstNFinalize(ArityType arity);

    value::TagValueMaybeOwned builtinAggLastN(ArityType arity);
    value::TagValueMaybeOwned builtinAggLastNMerge(ArityType arity);
    value::TagValueMaybeOwned builtinAggLastNFinalize(ArityType arity);

    template <TopBottomSense Sense>
    static int32_t aggTopBottomNAdd(value::Array* state,
                                    value::Array* array,
                                    size_t maxSize,
                                    int32_t memUsage,
                                    int32_t memLimit,
                                    ByteCode::TopBottomArgs& args);
    int32_t aggTopNAdd(value::Array* state,
                       value::Array* array,
                       size_t maxSize,
                       int32_t memUsage,
                       int32_t memLimit,
                       TopBottomArgs& args);
    int32_t aggBottomNAdd(value::Array* state,
                          value::Array* array,
                          size_t maxSize,
                          int32_t memUsage,
                          int32_t memLimit,
                          TopBottomArgs& args);
    template <TopBottomSense>
    value::TagValueMaybeOwned builtinAggTopBottomN(ArityType arity);
    template <TopBottomSense Sense, bool ValueIsArray>
    value::TagValueMaybeOwned builtinAggTopBottomNImpl(ArityType arity);
    template <TopBottomSense>
    value::TagValueMaybeOwned builtinAggTopBottomNArray(ArityType arity);
    template <TopBottomSense>
    value::TagValueMaybeOwned builtinAggTopBottomNMerge(ArityType arity);
    value::TagValueMaybeOwned builtinAggTopBottomNFinalize(ArityType arity);

    template <AccumulatorMinMaxN::MinMaxSense S>
    value::TagValueMaybeOwned builtinAggMinMaxN(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    value::TagValueMaybeOwned builtinAggMinMaxNMerge(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    value::TagValueMaybeOwned builtinAggMinMaxNFinalize(ArityType arity);

    value::TagValueMaybeOwned builtinAggRank(ArityType arity);
    value::TagValueMaybeOwned builtinAggRankColl(ArityType arity);
    value::TagValueMaybeOwned builtinAggDenseRank(ArityType arity);
    value::TagValueMaybeOwned builtinAggDenseRankColl(ArityType arity);
    value::TagValueMaybeOwned builtinAggRankFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggExpMovingAvg(ArityType arity);
    value::TagValueMaybeOwned builtinAggExpMovingAvgFinalize(ArityType arity);
    template <int sign>
    value::TagValueMaybeOwned builtinAggRemovableSum(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSumFinalize(ArityType arity);
    template <int sign>
    void aggRemovableSumImpl(value::Array* state, value::TypeTags rhsTag, value::Value rhsVal);
    FastTuple<bool, value::TypeTags, value::Value> aggRemovableSumFinalizeImpl(value::Array* state);
    template <class T, int sign>
    void updateRemovableSumAccForIntegerType(value::Array* sumAcc,
                                             value::TypeTags rhsTag,
                                             value::Value rhsVal);
    value::TagValueMaybeOwned builtinAggIntegralInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggIntegralAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggIntegralRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggIntegralFinalize(ArityType arity);
    value::TagValueMaybeOwned integralOfTwoPointsByTrapezoidalRule(
        value::TagValueView prevInput,
        value::TagValueView prevSortByVal,
        value::TagValueView newInput,
        value::TagValueView newSortByVal);
    value::TagValueMaybeOwned builtinAggDerivativeFinalize(ArityType arity);
    value::TagValueMaybeOwned aggRemovableAvgFinalizeImpl(value::Array* sumState, int64_t count);
    value::TagValueMaybeOwned builtinAggCovarianceAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggCovarianceRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggCovarianceFinalize(ArityType arity, bool isSamp);
    value::TagValueMaybeOwned builtinAggCovarianceSampFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggCovariancePopFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovablePushAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovablePushRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovablePushFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableConcatArraysInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableConcatArraysAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableConcatArraysRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableConcatArraysFinalize(ArityType arity);
    template <int quantity>
    void aggRemovableStdDevImpl(value::TypeTags stateTag,
                                value::Value stateVal,
                                value::TypeTags inputTag,
                                value::Value inputVal);
    value::TagValueMaybeOwned builtinAggRemovableStdDevAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableStdDevRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableStdDevFinalize(ArityType arity, bool isSamp);
    value::TagValueMaybeOwned builtinAggRemovableStdDevSampFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableStdDevPopFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableAvgFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstLastNInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstLastNAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggFirstLastNRemove(ArityType arity);
    template <AccumulatorFirstLastN::Sense S>
    value::TagValueMaybeOwned builtinAggFirstLastNFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggLinearFillCanAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggLinearFillAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggLinearFillFinalize(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSetCommonInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSetCommonCollInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableAddToSetAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableAddToSetRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSetUnionAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSetUnionRemove(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableSetCommonFinalize(ArityType arity);
    value::TagValueMaybeOwned aggRemovableMinMaxNInitImpl(CollatorInterface* collator);
    value::TagValueMaybeOwned builtinAggRemovableMinMaxNCollInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableMinMaxNInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableMinMaxNAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableMinMaxNRemove(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    value::TagValueMaybeOwned builtinAggRemovableMinMaxNFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtin(ArityType arity);
    value::TagValueMaybeOwned linearFillInterpolate(value::TagValueView x1,
                                                    value::TagValueView y1,
                                                    value::TagValueView x2,
                                                    value::TagValueView y2,
                                                    value::TagValueView x);
    value::TagValueMaybeOwned builtinAggRemovableTopBottomNInit(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableTopBottomNAdd(ArityType arity);
    value::TagValueMaybeOwned builtinAggRemovableTopBottomNRemove(ArityType arity);
    template <TopBottomSense>
    value::TagValueMaybeOwned builtinAggRemovableTopBottomNFinalize(ArityType arity);

    // Block builtins

    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockExists(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockTypeMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockIsTimezone(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockFillEmpty(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockFillEmptyBlock(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockFillType(ArityType arity);
    template <bool less>
    FastTuple<bool, value::TypeTags, value::Value> valueBlockMinMaxImpl(
        value::ValueBlock* inputBlock, value::ValueBlock* bitsetBlock);
    template <bool less>
    FastTuple<bool, value::TypeTags, value::Value> valueBlockAggMinMaxImpl(
        value::TypeTags accTag,
        value::Value accVal,
        value::TypeTags inputTag,
        value::Value inputVal,
        value::TypeTags bitsetTag,
        value::Value bitsetVal);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggMin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggMax(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggCount(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggSum(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggDoubleDoubleSum(
        ArityType arity);

    // Take advantage of the fact that we know we have block input, instead of looping over
    // generalized helper functions.
    template <TopBottomSense Sense, bool ValueIsArray>
    FastTuple<bool, value::TypeTags, value::Value> blockNativeAggTopBottomNImpl(
        value::TypeTags stateTag,
        value::Value stateVal,
        value::ValueBlock* bitsetBlock,
        SortSpec* sortSpec,
        size_t numKeysBlocks,
        size_t numValuesBlocks);

    template <TopBottomSense Sense, bool ValueIsArray>
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggTopBottomNImpl(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggTopN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggBottomN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggTopNArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggBottomNArray(
        ArityType arity);

    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockBlockArithmeticOperation(
        const value::TypeTags* bitsetTags,
        const value::Value* bitsetVals,
        value::ValueBlock* leftInputBlock,
        value::ValueBlock* rightInputBlock,
        size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockBlockArithmeticOperation(
        value::ValueBlock* leftInputBlock, value::ValueBlock* rightInputBlock, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinScalarBlockArithmeticOperation(
        const value::TypeTags* bitsetTags,
        const value::Value* bitsetVals,
        value::TagValueView scalar,
        value::ValueBlock* block,
        size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinScalarBlockArithmeticOperation(
        value::TagValueView scalar, value::ValueBlock* block, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockScalarArithmeticOperation(
        const value::TypeTags* bitsetTags,
        const value::Value* bitsetVals,
        value::ValueBlock* block,
        value::TagValueView scalar,
        size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockScalarArithmeticOperation(
        value::ValueBlock* block, value::TagValueView scalar, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinScalarScalarArithmeticOperation(
        value::TagValueView leftInputScalar, value::TagValueView rightInputScalar, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockArithmeticOperation(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockSub(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockMult(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockDiv(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockDateDiff(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockDateTrunc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockDateAdd(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockRound(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockTrunc(ArityType arity);

    template <class Cmp, value::ColumnOpType::Flags AddFlags = value::ColumnOpType::kNoFlags>
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockCmpScalar(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGtScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGteScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockEqScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockNeqScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLtScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLteScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockCmp3wScalar(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockCombine(ArityType arity);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLogicalOperation(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLogicalAnd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLogicalOr(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockLogicalNot(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockNewFill(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockSize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockNone(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockIsMember(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockCoerceToBool(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockMod(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockConvert(ArityType arity);
    template <bool IsAscending>
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGetSortKey(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGetSortKeyAsc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGetSortKeyDesc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGetNonLeafSortKeyAsc(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockGetNonLeafSortKeyDesc(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCellFoldValues_F(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCellFoldValues_P(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCellBlockGetFlatValuesBlock(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCurrentDate(ArityType arity);

    /**
     * Dispatcher for calls to VM built-in C++ functions enumerated by enum class Builtin.
     */
    FastTuple<bool, value::TypeTags, value::Value> dispatchBuiltin(Builtin f,
                                                                   ArityType arity,
                                                                   const CodeFragment* code);

    static constexpr size_t offsetOwned = 0;
    static constexpr size_t offsetTag = 1;
    static constexpr size_t offsetVal = 2;

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    FastTuple<bool, value::TypeTags, value::Value> readTuple(uint8_t* ptr) noexcept {
        auto owned = readFromMemory<bool>(ptr + offsetOwned);
        auto tag = readFromMemory<value::TypeTags>(ptr + offsetTag);
        auto val = readFromMemory<value::Value>(ptr + offsetVal);
        return {owned, tag, val};
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void writeTuple(uint8_t* ptr, bool owned, value::TypeTags tag, value::Value val) noexcept {
        writeToMemory(ptr + offsetOwned, owned);
        writeToMemory(ptr + offsetTag, tag);
        writeToMemory(ptr + offsetVal, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    FastTuple<bool, value::TypeTags, value::Value> getFromStack(size_t offset, bool pop = false) {
        auto ret = readTuple(_argStackTop - offset * sizeOfElement);

        if (pop) {
            popStack();
        }

        return ret;
    }

    /**
     * Returns the value triple at the top of the VM stack, whether the stack owned it or not. If
     * the stack DID own it, this transfers ownership to the caller (like a move). If the stack did
     * NOT own it, no ownership transfer occurs, so something else still owns it.
     *
     * If you want the caller always to own the returned value, call moveOwnedFromStack() instead.
     */
    MONGO_COMPILER_ALWAYS_INLINE_OPT
    FastTuple<bool, value::TypeTags, value::Value> moveFromStack(size_t offset) noexcept {
        if (MONGO_likely(offset == 0)) {
            auto [owned, tag, val] = readTuple(_argStackTop);
            writeToMemory(_argStackTop + offsetOwned, false);
            return {owned, tag, val};
        } else {
            auto ptr = _argStackTop - offset * sizeOfElement;
            auto [owned, tag, val] = readTuple(ptr);
            writeToMemory(ptr + offsetOwned, false);
            return {owned, tag, val};
        }
    }

    /**
     * Returns the value triple at the top of the VM stack, whether the stack owned it or not, and
     * also causes the caller to own it and the stack not to own it. If the stack DID own it, this
     * transfers ownership to the caller (like a move). If the stack did NOT own it, this makes a
     * copy and gives ownership of the copy to the caller, while something else still owns the
     * original on the top of the stack.
     *
     * If you do not want ownership to transfer to the caller if the stack did not already own it,
     * call moveFromStack() instead.
     */
    MONGO_COMPILER_ALWAYS_INLINE_OPT
    std::pair<value::TypeTags, value::Value> moveOwnedFromStack(size_t offset) {
        auto [owned, tag, val] = moveFromStack(offset);
        if (!owned) {
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        return {tag, val};
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void setTagToNothing(size_t offset) noexcept {
        if (MONGO_likely(offset == 0)) {
            writeToMemory(_argStackTop + offsetTag, value::TypeTags::Nothing);
        } else {
            auto ptr = _argStackTop - offset * sizeOfElement;
            writeToMemory(ptr + offsetTag, value::TypeTags::Nothing);
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void setStack(size_t offset, bool owned, value::TypeTags tag, value::Value val) noexcept {
        if (MONGO_likely(offset == 0)) {
            topStack(owned, tag, val);
        } else {
            writeTuple(_argStackTop - offset * sizeOfElement, owned, tag, val);
        }
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void pushStack(bool owned, value::TypeTags tag, value::Value val) {
        dassert(_argStackEnd - _argStackTop >= static_cast<std::ptrdiff_t>(sizeOfElement),
                "Invalid pushStack call leads to overflow");
        auto localPtr = _argStackTop += sizeOfElement;

        writeTuple(localPtr, owned, tag, val);
    }

    /**
     * Overwrites the current value at the top of the stack with the value triple passed in.
     */
    MONGO_COMPILER_ALWAYS_INLINE void topStack(bool owned,
                                               value::TypeTags tag,
                                               value::Value val) noexcept {
        writeTuple(_argStackTop, owned, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void popStack() {
        _argStackTop -= sizeOfElement;
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void popAndReleaseStack() {
        auto [owned, tag, val] = getFromStack(0);
        if (owned) {
            value::releaseValue(tag, val);
        }

        popStack();
    }

    void stackReset() noexcept {
        _argStackTop = _argStack - sizeOfElement;
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT void allocStack(size_t size) noexcept {
        auto newSizeDelta = size * sizeOfElement;
        if (_argStackEnd <= _argStackTop + newSizeDelta) {
            allocStackImpl(newSizeDelta);
        }
    }

    void allocStackImpl(size_t newSizeDelta) noexcept;

    void swapStack();

    // The top entry in '_argStack', or one element before the stack when empty.
    uint8_t* _argStackTop{nullptr};

    // The byte following '_argStack's current memory block.
    uint8_t* _argStackEnd{nullptr};

    // Expression execution stack of (owned, tag, value) tuples each of 'sizeOfElement' bytes.
    uint8_t* _argStack{nullptr};
};

class ByteCode::MakeObjImplBase {
public:
    MONGO_COMPILER_ALWAYS_INLINE MakeObjImplBase(ByteCode& bc,
                                                 int argsStackOffset,
                                                 const CodeFragment* code)
        : bc(bc), argsStackOffset(argsStackOffset), code(code) {}

protected:
    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> getSpec() const {
        return bc.getFromStack(0);
    }

    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> getInputObject()
        const {
        return bc.getFromStack(1);
    }

    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> extractInputObject()
        const {
        return bc.moveFromStack(1);
    }

    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> getArg(
        size_t argIdx) const {
        return bc.getFromStack(argsStackOffset + argIdx);
    }

    // Invokes the specified lambda, passing in a view of the specified input value.
    MONGO_COMPILER_ALWAYS_INLINE FastTuple<bool, value::TypeTags, value::Value> invokeLambda(
        int64_t lamPos, value::TypeTags inputTag, value::Value inputVal) const {
        // Invoke the lambda.
        bc.pushStack(false, inputTag, inputVal);
        bc.runLambdaInternal(code, lamPos);
        // Move the result off the stack and return it.
        auto outputTuple = bc.getFromStack(0);
        bc.popStack();
        return outputTuple;
    }

private:
    ByteCode& bc;
    const int argsStackOffset;
    const CodeFragment* const code;
};

struct ByteCode::InvokeLambdaFunctor {
    InvokeLambdaFunctor(ByteCode& bytecode, const CodeFragment* code, int64_t lamPos)
        : bytecode(bytecode), code(code), lamPos(lamPos) {}

    std::pair<value::TypeTags, value::Value> operator()(value::TypeTags tag,
                                                        value::Value val) const {
        // Invoke the lambda.
        bytecode.pushStack(false, tag, val);
        bytecode.runLambdaInternal(code, lamPos);
        // Move the result off the stack, make sure it's owned, and return it.
        auto result = bytecode.moveOwnedFromStack(0);
        bytecode.popStack();
        return result;
    }

    ByteCode& bytecode;
    const CodeFragment* const code;
    const int64_t lamPos;
};

class ByteCode::TopBottomArgs {
public:
    TopBottomArgs(TopBottomSense sense,
                  SortSpec* sortSpec,
                  bool decomposedKey,
                  bool decomposedValue)
        : _sortSpec(sortSpec),
          _sense(sense),
          _decomposedKey(decomposedKey),
          _decomposedValue(decomposedValue) {}

    // Add definition to vm.cpp for this method.
    virtual ~TopBottomArgs();

    bool keySortsBefore(value::TagValueView item) {
        if (!_keyArg) {
            return keySortsBeforeImpl(item);
        } else {
            auto [cmpTag, cmpVal] =
                _sortSpec->compare(_keyArg->tag(), _keyArg->value(), item.tag, item.value);
            if (cmpTag == value::TypeTags::NumberInt32) {
                int32_t cmp = value::bitcastTo<int32_t>(cmpVal);
                return _sense == TopBottomSense::kTop ? cmp < 0 : cmp > 0;
            }
            return false;
        }
    }

    value::TagValueOwned getOwnedKey() {
        if (!_keyArg) {
            return getOwnedKeyImpl();
        } else {
            return _keyArg->moveToOwned();
        }
    }

    value::TagValueOwned getOwnedValue() {
        if (!_valueArg) {
            return getOwnedValueImpl();
        } else {
            return _valueArg->moveToOwned();
        }
    }

    SortSpec* getSortSpec() const {
        return _sortSpec;
    }
    TopBottomSense getTopBottomSense() const {
        return _sense;
    }

protected:
    template <TopBottomSense Sense>
    static int32_t compare(value::TypeTags leftElemTag,
                           value::Value leftElemVal,
                           value::TypeTags rightElemTag,
                           value::Value rightElemVal) {
        auto [cmpTag, cmpVal] =
            value::compareValue(leftElemTag, leftElemVal, rightElemTag, rightElemVal);

        if (cmpTag == value::TypeTags::NumberInt32) {
            int32_t cmp = value::bitcastTo<int32_t>(cmpVal);
            return Sense == TopBottomSense::kTop ? cmp : -cmp;
        }

        return 0;
    }

    virtual bool keySortsBeforeImpl(value::TagValueView item) = 0;
    virtual value::TagValueOwned getOwnedKeyImpl() = 0;
    virtual value::TagValueOwned getOwnedValueImpl() = 0;

    void setDirectKeyArg(value::TagValueMaybeOwned arg) {
        _keyArg.emplace(std::move(arg));
    }

    void setDirectValueArg(value::TagValueMaybeOwned arg) {
        _valueArg.emplace(std::move(arg));
    }

    SortSpec* _sortSpec = nullptr;
    TopBottomSense _sense;
    bool _decomposedKey = false;
    bool _decomposedValue = false;
    boost::optional<value::TagValueMaybeOwned> _keyArg;
    boost::optional<value::TagValueMaybeOwned> _valueArg;
};
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
