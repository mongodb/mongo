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

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <absl/hash/hash.h>
#include <boost/optional/optional.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/sort_spec.h"
#include "mongo/db/exec/sbe/values/column_op.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/datetime.h"
#include "mongo/db/exec/sbe/vm/label.h"
#include "mongo/db/exec/sbe/vm/makeobj_cursors.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#if !defined(MONGO_CONFIG_DEBUG_BUILD)
#define MONGO_COMPILER_ALWAYS_INLINE_OPT MONGO_COMPILER_ALWAYS_INLINE
#else
#define MONGO_COMPILER_ALWAYS_INLINE_OPT
#endif

namespace mongo {
namespace sbe {
namespace vm {
namespace {
template <typename T>
T readFromMemory(const uint8_t* ptr) noexcept {
    static_assert(!IsEndian<T>::value);

    T val;
    memcpy(&val, ptr, sizeof(T));
    return val;
}

template <typename T>
size_t writeToMemory(uint8_t* ptr, const T val) noexcept {
    static_assert(!IsEndian<T>::value);

    memcpy(ptr, &val, sizeof(T));
    return sizeof(T);
}
}  // namespace

/**
 * Enumeration of built-in VM instructions. These are implemented in vm.cpp ByteCode::runInternal.
 *
 * See also enum class Builtin for built-in functions, like 'addToArray', that are implemented as
 * C++ rather than VM instructions.
 */
struct Instruction {
    enum Tags {
        pushConstVal,
        pushAccessVal,
        pushOwnedAccessorVal,
        pushEnvAccessorVal,
        pushMoveVal,
        pushLocalVal,
        pushMoveLocalVal,
        pushLocalLambda,
        pop,
        swap,

        add,
        sub,
        mul,
        div,
        idiv,
        mod,
        negate,
        numConvert,

        logicNot,

        less,
        lessEq,
        greater,
        greaterEq,
        eq,
        neq,

        // 3 way comparison (spaceship) with bson woCompare semantics.
        cmp3w,

        // collation-aware comparison instructions
        collLess,
        collLessEq,
        collGreater,
        collGreaterEq,
        collEq,
        collNeq,
        collCmp3w,

        fillEmpty,
        fillEmptyImm,
        getField,
        getFieldImm,
        getElement,
        collComparisonKey,
        getFieldOrElement,
        traverseP,  // traverse projection paths
        traversePImm,
        traverseF,  // traverse filter paths
        traverseFImm,
        magicTraverseF,
        // Iterates over values in column index cells. Skips values from nested arrays.
        traverseCsiCellValues,
        // Iterates the column index cell and returns values representing the types of cell's
        // content, including arrays and nested objects. Skips contents of nested arrays.
        traverseCsiCellTypes,
        setField,      // add or overwrite a field in a document
        getArraySize,  // number of elements

        aggSum,
        aggMin,
        aggMax,
        aggFirst,
        aggLast,

        aggCollMin,
        aggCollMax,

        exists,
        isNull,
        isObject,
        isArray,
        isInListData,
        isString,
        isNumber,
        isBinData,
        isDate,
        isNaN,
        isInfinity,
        isRecordId,
        isMinKey,
        isMaxKey,
        isTimestamp,
        typeMatchImm,

        function,
        functionSmall,

        jmp,  // offset is calculated from the end of instruction
        jmpTrue,
        jmpFalse,
        jmpNothing,
        jmpNotNothing,
        ret,  // used only by simple local lambdas
        allocStack,

        fail,

        dateTruncImm,

        valueBlockApplyLambda,  // Applies a lambda to each element in a block, returning a new
                                // block.

        lastInstruction  // this is just a marker used to calculate number of instructions
    };

    enum Constants : uint8_t {
        Nothing,
        Null,
        False,
        True,
        Int32One,
    };

    constexpr static size_t kMaxInlineStringSize = 256;

    /**
     * An instruction parameter descriptor. Values (instruction arguments) live on the VM stack and
     * the descriptor tells where to find it. The position on the stack is expressed as an offset
     * from the top of stack.
     * Optionally, an instruction can "consume" the value by popping the stack. All non-named
     * temporaries are popped after the use. Naturally, only the top of stack (offset 0) can be
     * popped. We do not support an arbitrary erasure from the middle of stack.
     */
    struct Parameter {
        int variable{0};
        bool moveFrom{false};
        boost::optional<FrameId> frameId;

        // Get the size in bytes of an instruction parameter encoded in byte code.
        size_t size() const noexcept {
            return sizeof(bool) + (frameId ? sizeof(int) : 0);
        }

        MONGO_COMPILER_ALWAYS_INLINE_OPT
        static FastTuple<bool, bool, int> decodeParam(const uint8_t*& pcPointer) noexcept {
            auto flags = readFromMemory<uint8_t>(pcPointer);
            bool pop = flags & 1u;
            bool moveFrom = flags & 2u;
            pcPointer += sizeof(pop);
            int offset = 0;
            if (!pop) {
                offset = readFromMemory<int>(pcPointer);
                pcPointer += sizeof(offset);
            }

            return {pop, moveFrom, offset};
        }
    };

    static const char* toStringConstants(Constants k) {
        switch (k) {
            case Nothing:
                return "Nothing";
            case Null:
                return "Null";
            case True:
                return "True";
            case False:
                return "False";
            case Int32One:
                return "1";
            default:
                return "unknown";
        }
    }

    // Make sure that values in this arrays are always in-sync with the enum.
    static int stackOffset[];

    uint8_t tag;

    const char* toString() const {
        switch (tag) {
            case pushConstVal:
                return "pushConstVal";
            case pushAccessVal:
                return "pushAccessVal";
            case pushOwnedAccessorVal:
                return "pushOwnedAccessorVal";
            case pushEnvAccessorVal:
                return "pushEnvAccessorVal";
            case pushMoveVal:
                return "pushMoveVal";
            case pushLocalVal:
                return "pushLocalVal";
            case pushMoveLocalVal:
                return "pushMoveLocalVal";
            case pushLocalLambda:
                return "pushLocalLambda";
            case pop:
                return "pop";
            case swap:
                return "swap";
            case add:
                return "add";
            case sub:
                return "sub";
            case mul:
                return "mul";
            case div:
                return "div";
            case idiv:
                return "idiv";
            case mod:
                return "mod";
            case negate:
                return "negate";
            case numConvert:
                return "numConvert";
            case logicNot:
                return "logicNot";
            case less:
                return "less";
            case lessEq:
                return "lessEq";
            case greater:
                return "greater";
            case greaterEq:
                return "greaterEq";
            case eq:
                return "eq";
            case neq:
                return "neq";
            case cmp3w:
                return "cmp3w";
            case collLess:
                return "collLess";
            case collLessEq:
                return "collLessEq";
            case collGreater:
                return "collGreater";
            case collGreaterEq:
                return "collGreaterEq";
            case collEq:
                return "collEq";
            case collNeq:
                return "collNeq";
            case collCmp3w:
                return "collCmp3w";
            case fillEmpty:
                return "fillEmpty";
            case fillEmptyImm:
                return "fillEmptyImm";
            case getField:
                return "getField";
            case getFieldImm:
                return "getFieldImm";
            case getElement:
                return "getElement";
            case collComparisonKey:
                return "collComparisonKey";
            case getFieldOrElement:
                return "getFieldOrElement";
            case traverseP:
                return "traverseP";
            case traversePImm:
                return "traversePImm";
            case traverseF:
                return "traverseF";
            case traverseFImm:
                return "traverseFImm";
            case traverseCsiCellValues:
                return "traverseCsiCellValues";
            case traverseCsiCellTypes:
                return "traverseCsiCellTypes";
            case setField:
                return "setField";
            case getArraySize:
                return "getArraySize";
            case aggSum:
                return "aggSum";
            case aggMin:
                return "aggMin";
            case aggMax:
                return "aggMax";
            case aggFirst:
                return "aggFirst";
            case aggLast:
                return "aggLast";
            case aggCollMin:
                return "aggCollMin";
            case aggCollMax:
                return "aggCollMax";
            case exists:
                return "exists";
            case isNull:
                return "isNull";
            case isObject:
                return "isObject";
            case isArray:
                return "isArray";
            case isInListData:
                return "isInListData";
            case isString:
                return "isString";
            case isNumber:
                return "isNumber";
            case isBinData:
                return "isBinData";
            case isDate:
                return "isDate";
            case isNaN:
                return "isNaN";
            case isInfinity:
                return "isInfinity";
            case isRecordId:
                return "isRecordId";
            case isMinKey:
                return "isMinKey";
            case isMaxKey:
                return "isMaxKey";
            case isTimestamp:
                return "isTimestamp";
            case typeMatchImm:
                return "typeMatchImm";
            case function:
                return "function";
            case functionSmall:
                return "functionSmall";
            case jmp:
                return "jmp";
            case jmpTrue:
                return "jmpTrue";
            case jmpFalse:
                return "jmpFalse";
            case jmpNothing:
                return "jmpNothing";
            case jmpNotNothing:
                return "jmpNotNothing";
            case ret:
                return "ret";
            case allocStack:
                return "allocStack";
            case fail:
                return "fail";
            case dateTruncImm:
                return "dateTruncImm";
            default:
                return "unrecognized";
        }
    }
};
static_assert(sizeof(Instruction) == sizeof(uint8_t));

/**
 * Enumeration of SBE VM built-in functions. These are dispatched by ByteCode::dispatchBuiltin() in
 * vm.cpp. An enum value 'foo' refers to a C++ implementing function named builtinFoo().
 *
 * See also struct Instruction for "functions" like 'setField' that are implemented as single VM
 * instructions.
 *
 * Builtins which can fit into one byte and have small arity are encoded using a special instruction
 * tag, functionSmall.
 */
using SmallBuiltinType = uint8_t;
enum class Builtin : uint16_t {
    split,
    regexMatch,
    replaceOne,  // replace first occurrence of a specified substring with a diffferent substring
    dateDiff,
    dateParts,
    dateToParts,
    isoDateToParts,
    dayOfYear,
    dayOfMonth,
    dayOfWeek,
    datePartsWeekYear,
    dateToString,
    dateFromString,
    dateFromStringNoThrow,
    dropFields,
    newArray,  // create a new array from the top 'arity' values on the stack
    keepFields,
    newArrayFromRange,
    newObj,      // create a new object from 'arity' alternating field names and values on the stack
    newBsonObj,  // same as 'newObj', except it creates a BSON object
    ksToString,  // KeyString to string
    newKs,       // new KeyString
    collNewKs,   // new KeyString (with collation)
    abs,         // absolute value
    ceil,
    floor,
    trunc,
    exp,
    ln,
    log10,
    sqrt,
    pow,
    addToArray,        // agg function to append to an array
    addToArrayCapped,  // agg function to append to an array, fails when the array reaches specified
                       // size
    mergeObjects,      // agg function to merge BSON documents
    addToSet,          // agg function to append to a set
    addToSetCapped,    // agg function to append to a set, fails when the set reaches specified size
    collAddToSet,      // agg function to append to a set (with collation)
    collAddToSetCapped,  // agg function to append to a set (with collation), fails when the set
                         // reaches specified size

    // Special double summation.
    doubleDoubleSum,
    // Accumulator to merge simple sums into a double double summation.
    convertSimpleSumToDoubleDoubleSum,
    // A variant of the standard sum aggregate function which maintains a DoubleDouble as the
    // accumulator's underlying state.
    aggDoubleDoubleSum,
    // Converts a DoubleDouble sum into a single numeric scalar for use once the summation is
    // complete.
    doubleDoubleSumFinalize,
    // Converts a partial sum into a format suitable for serialization over the wire to the merging
    // node. The merging node expects the internal state of the DoubleDouble summation to be
    // serialized in a particular format.
    doubleDoublePartialSumFinalize,
    // An agg function which can be used to sum a sequence of DoubleDouble inputs, producing the
    // resulting total as a DoubleDouble.
    aggMergeDoubleDoubleSums,

    // Implements Welford's online algorithm for computing sample or population standard deviation
    // in a single pass.
    aggStdDev,
    // Combines standard deviations that have been partially computed on a subset of the data
    // using Welford's online algorithm.
    aggMergeStdDevs,

    stdDevPopFinalize,
    stdDevSampFinalize,
    bitTestZero,      // test bitwise mask & value is zero
    bitTestMask,      // test bitwise mask & value is mask
    bitTestPosition,  // test BinData with a bit position list
    bsonSize,         // implements $bsonSize
    strLenBytes,
    toUpper,
    toLower,
    coerceToBool,
    coerceToString,
    concat,
    concatArrays,
    trim,
    ltrim,
    rtrim,

    // Agg function to concatenate arrays, failing when the accumulator reaches a specified size.
    aggConcatArraysCapped,

    // Agg functions to compute the set union of two arrays (no size cap).
    aggSetUnion,
    aggCollSetUnion,
    // Agg functions to compute the set union of two arrays (with a size cap).
    aggSetUnionCapped,
    aggCollSetUnionCapped,

    acos,
    acosh,
    asin,
    asinh,
    atan,
    atanh,
    atan2,
    cos,
    cosh,
    degreesToRadians,
    radiansToDegrees,
    sin,
    sinh,
    tan,
    tanh,
    round,
    isMember,
    indexOfBytes,
    indexOfCP,
    isDayOfWeek,
    isTimeUnit,
    isTimezone,
    isValidToStringFormat,
    validateFromStringFormat,
    setUnion,
    setIntersection,
    setDifference,
    setEquals,
    setIsSubset,
    collSetUnion,
    collSetIntersection,
    collSetDifference,
    collSetEquals,
    collSetIsSubset,
    runJsPredicate,
    regexCompile,  // compile <pattern, options> into value::pcreRegex
    regexFind,
    regexFindAll,
    shardFilter,
    shardHash,
    extractSubArray,
    isArrayEmpty,
    reverseArray,
    sortArray,
    dateAdd,
    hasNullBytes,
    getRegexPattern,
    getRegexFlags,
    hash,
    ftsMatch,
    generateSortKey,
    generateCheapSortKey,
    sortKeyComponentVectorGetElement,
    sortKeyComponentVectorToArray,

    makeBsonObj,
    tsSecond,
    tsIncrement,
    typeMatch,
    dateTrunc,
    internalLeast,     // helper functions for computation of sort keys
    internalGreatest,  // helper functions for computation of sort keys
    year,
    month,
    hour,
    minute,
    second,
    millisecond,
    week,
    isoWeekYear,
    isoDayOfWeek,
    isoWeek,
    objectToArray,
    setToArray,
    arrayToObject,

    aggFirstNNeedsMoreInput,
    aggFirstN,
    aggFirstNMerge,
    aggFirstNFinalize,
    aggLastN,
    aggLastNMerge,
    aggLastNFinalize,
    aggTopN,
    aggTopNMerge,
    aggTopNFinalize,
    aggBottomN,
    aggBottomNMerge,
    aggBottomNFinalize,
    aggMaxN,
    aggMaxNMerge,
    aggMaxNFinalize,
    aggMinN,
    aggMinNMerge,
    aggMinNFinalize,
    aggRank,
    aggRankColl,
    aggDenseRank,
    aggDenseRankColl,
    aggRankFinalize,
    aggExpMovingAvg,
    aggExpMovingAvgFinalize,
    aggRemovableSumAdd,
    aggRemovableSumRemove,
    aggRemovableSumFinalize,
    aggIntegralInit,
    aggIntegralAdd,
    aggIntegralRemove,
    aggIntegralFinalize,
    aggDerivativeFinalize,
    aggCovarianceAdd,
    aggCovarianceRemove,
    aggCovarianceSampFinalize,
    aggCovariancePopFinalize,
    aggRemovablePushAdd,
    aggRemovablePushRemove,
    aggRemovablePushFinalize,
    aggRemovableStdDevAdd,
    aggRemovableStdDevRemove,
    aggRemovableStdDevSampFinalize,
    aggRemovableStdDevPopFinalize,
    aggRemovableAvgFinalize,
    aggLinearFillCanAdd,
    aggLinearFillAdd,
    aggLinearFillFinalize,
    aggRemovableFirstNInit,
    aggRemovableFirstNAdd,
    aggRemovableFirstNRemove,
    aggRemovableFirstNFinalize,
    aggRemovableLastNInit,
    aggRemovableLastNAdd,
    aggRemovableLastNRemove,
    aggRemovableLastNFinalize,
    aggRemovableAddToSetInit,
    aggRemovableAddToSetCollInit,
    aggRemovableAddToSetAdd,
    aggRemovableAddToSetRemove,
    aggRemovableAddToSetFinalize,
    aggRemovableMinMaxNCollInit,
    aggRemovableMinMaxNInit,
    aggRemovableMinMaxNAdd,
    aggRemovableMinMaxNRemove,
    aggRemovableMinNFinalize,
    aggRemovableMaxNFinalize,
    aggRemovableTopNInit,
    aggRemovableTopNAdd,
    aggRemovableTopNRemove,
    aggRemovableTopNFinalize,
    aggRemovableBottomNInit,
    aggRemovableBottomNAdd,
    aggRemovableBottomNRemove,
    aggRemovableBottomNFinalize,


    // Additional one-byte builtins go here.

    // Start of 2 byte builtins.
    valueBlockExists = 256,
    valueBlockFillEmpty,
    valueBlockFillEmptyBlock,
    valueBlockAggMin,
    valueBlockAggMax,
    valueBlockAggCount,
    valueBlockDateDiff,
    valueBlockDateTrunc,
    valueBlockDateAdd,
    valueBlockTrunc,
    valueBlockRound,
    valueBlockAggSum,
    valueBlockAdd,
    valueBlockSub,
    valueBlockMult,
    valueBlockDiv,
    valueBlockGtScalar,
    valueBlockGteScalar,
    valueBlockEqScalar,
    valueBlockNeqScalar,
    valueBlockLtScalar,
    valueBlockLteScalar,
    valueBlockCmp3wScalar,
    valueBlockCombine,
    valueBlockLogicalAnd,
    valueBlockLogicalOr,
    valueBlockLogicalNot,
    valueBlockNewFill,
    valueBlockSize,
    valueBlockNone,
    valueBlockIsMember,
    valueBlockCoerceToBool,
    valueBlockMod,
    valueBlockConvert,

    cellFoldValues_F,
    cellFoldValues_P,
    cellBlockGetFlatValuesBlock,
};

std::string builtinToString(Builtin b);

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
 * - The element at index `kIsGroupAccum` specifices if the accumulator belongs to group-by stage
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

struct MakeObjStackOffsets {
    int fieldsStackOffset = 0;
    int argsStackOffset = 0;
};

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
 * - The element at index `kCount` keeps track of the total number of values processd
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

using SmallArityType = uint8_t;
using ArityType = uint32_t;

class CodeFragment {
public:
    const auto& frames() const {
        return _frames;
    }
    auto& instrs() {
        return _instrs;
    }
    const auto& instrs() const {
        return _instrs;
    }
    auto stackSize() const {
        return _stackSize;
    }
    auto maxStackSize() const {
        return _maxStackSize;
    }

    void append(CodeFragment&& code);
    void appendNoStack(CodeFragment&& code);
    // Used when either `lhs` or `rhs` will run, but not both. This method will adjust the stack
    // size once in this call, rather than twice (once for each CodeFragment). The CodeFragments
    // must have the same stack size for us to know how to adjust the stack at compile time.
    void append(CodeFragment&& lhs, CodeFragment&& rhs);
    void appendConstVal(value::TypeTags tag, value::Value val);
    void appendAccessVal(value::SlotAccessor* accessor);
    void appendMoveVal(value::SlotAccessor* accessor);
    void appendLocalVal(FrameId frameId, int variable, bool moveFrom);
    void appendLocalLambda(int codePosition);
    void appendPop();
    void appendSwap();
    void appendAdd(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendSub(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendMul(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendDiv(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendIDiv(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendMod(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendNegate(Instruction::Parameter input);
    void appendNot(Instruction::Parameter input);
    void appendLess(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendLessEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGreater(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGreaterEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendEq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendNeq(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendCmp3w(Instruction::Parameter lhs, Instruction::Parameter rhs);

    void appendCollLess(Instruction::Parameter lhs,
                        Instruction::Parameter rhs,
                        Instruction::Parameter collator);

    void appendCollLessEq(Instruction::Parameter lhs,
                          Instruction::Parameter rhs,
                          Instruction::Parameter collator);

    void appendCollGreater(Instruction::Parameter lhs,
                           Instruction::Parameter rhs,
                           Instruction::Parameter collator);

    void appendCollGreaterEq(Instruction::Parameter lhs,
                             Instruction::Parameter rhs,
                             Instruction::Parameter collator);

    void appendCollEq(Instruction::Parameter lhs,
                      Instruction::Parameter rhs,
                      Instruction::Parameter collator);

    void appendCollNeq(Instruction::Parameter lhs,
                       Instruction::Parameter rhs,
                       Instruction::Parameter collator);

    void appendCollCmp3w(Instruction::Parameter lhs,
                         Instruction::Parameter rhs,
                         Instruction::Parameter collator);

    void appendFillEmpty();
    void appendFillEmpty(Instruction::Constants k);
    void appendGetField(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGetField(Instruction::Parameter input, StringData fieldName);
    void appendGetElement(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendCollComparisonKey(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendGetFieldOrElement(Instruction::Parameter lhs, Instruction::Parameter rhs);
    void appendTraverseP();
    void appendTraverseP(int codePosition, Instruction::Constants k);
    void appendTraverseF();
    void appendTraverseF(int codePosition, Instruction::Constants k);
    void appendMagicTraverseF();
    void appendTraverseCellValues();
    void appendTraverseCellValues(int codePosition);
    void appendTraverseCellTypes();
    void appendTraverseCellTypes(int codePosition);
    void appendSetField();
    void appendGetArraySize(Instruction::Parameter input);
    void appendDateTrunc(TimeUnit unit, int64_t binSize, TimeZone timezone, DayOfWeek startOfWeek);
    void appendValueBlockApplyLambda();

    void appendSum();
    void appendMin();
    void appendMax();
    void appendFirst();
    void appendLast();
    void appendCollMin();
    void appendCollMax();
    void appendExists(Instruction::Parameter input);
    void appendIsNull(Instruction::Parameter input);
    void appendIsObject(Instruction::Parameter input);
    void appendIsArray(Instruction::Parameter input);
    void appendIsInListData(Instruction::Parameter input);
    void appendIsString(Instruction::Parameter input);
    void appendIsNumber(Instruction::Parameter input);
    void appendIsBinData(Instruction::Parameter input);
    void appendIsDate(Instruction::Parameter input);
    void appendIsNaN(Instruction::Parameter input);
    void appendIsInfinity(Instruction::Parameter input);
    void appendIsRecordId(Instruction::Parameter input);
    void appendIsMinKey(Instruction::Parameter input);
    void appendIsMaxKey(Instruction::Parameter input);
    void appendIsTimestamp(Instruction::Parameter input);
    void appendTypeMatch(Instruction::Parameter input, uint32_t mask);
    void appendFunction(Builtin f, ArityType arity);
    void appendLabelJump(LabelId labelId);
    void appendLabelJumpTrue(LabelId labelId);
    void appendLabelJumpFalse(LabelId labelId);
    void appendLabelJumpNothing(LabelId labelId);
    void appendLabelJumpNotNothing(LabelId labelId);
    void appendRet();
    void appendAllocStack(uint32_t size);
    void appendFail();
    void appendNumericConvert(value::TypeTags targetTag);

    // For printing from an interactive debugger.
    std::string toString() const;

    // Declares and defines a local variable frame at the current depth.
    // Local frame declaration is used to resolve the stack offsets of local variable access.
    // All references local variables must have matching frame declaration. The
    // variable reference and frame declaration is allowed to happen in any order.
    void declareFrame(FrameId frameId);

    // Declares and defines a local variable frame at the current stack depth modifies by the given
    // offset.
    void declareFrame(FrameId frameId, int stackOffset);

    // Removes the frame from scope. The frame must have no outstanding fixups.
    // That is: must be declared or never referenced.
    void removeFrame(FrameId frameId);

    // Returns whether the are any frames currently in scope.
    bool hasFrames() const;

    // Associates the current code position with a label.
    void appendLabel(LabelId labelId);

    // Removes the label from scope. The label must have no outstanding fixups.
    // That is: must be associated with code position or never referenced.
    void removeLabel(LabelId labelId);

    void validate();

private:
    // Adjusts all the stack offsets in the outstanding fixups by the provided delta as follows: for
    // a given 'stackOffsetDelta' of frames in this CodeFragment:
    //   1. Adds this delta to the 'stackPosition' of all frames having a defined stack position.
    //   2. Adds this delta to all uses of frame stack posn's in code (located at 'fixupOffset's).
    // The net effect is to change the stack offsets of all frames with defined stack positions and
    // all code references to frame offsets in this CodeFragment by 'stackOffsetDelta'.
    void fixupStackOffsets(int stackOffsetDelta);

    // Stores the fixup information for stack frames.
    // fixupOffsets - byte offsets in the code where the stack depth of the frame was used and need
    //   fixup.
    // stackPosition - stack depth in elements of where the frame was declared, or kPositionNotSet
    //   if not known yet.
    struct FrameInfo {
        static constexpr int64_t kPositionNotSet = std::numeric_limits<int64_t>::min();

        absl::InlinedVector<size_t, 2> fixupOffsets;
        int64_t stackPosition{kPositionNotSet};
    };

    // Stores the fixup information for labels.
    // fixupOffsets - offsets in the code where the label was used and need fixup.
    // definitionOffset - offset in the code where label was defined.
    struct LabelInfo {
        static constexpr int64_t kOffsetNotSet = std::numeric_limits<int64_t>::min();
        absl::InlinedVector<size_t, 2> fixupOffsets;
        int64_t definitionOffset{kOffsetNotSet};
    };

    template <typename... Ts>
    void appendSimpleInstruction(Instruction::Tags tag, Ts&&... params);
    void appendLabelJumpInstruction(LabelId labelId, Instruction::Tags tag);

    auto allocateSpace(size_t size) {
        auto oldSize = _instrs.size();
        _instrs.resize(oldSize + size);
        return _instrs.data() + oldSize;
    }

    template <typename... Ts>
    void adjustStackSimple(const Instruction& i, Ts&&... params);
    void copyCodeAndFixup(CodeFragment&& from);

    template <typename... Ts>
    size_t appendParameters(uint8_t* ptr, Ts&&... params);
    size_t appendParameter(uint8_t* ptr, Instruction::Parameter param, int& popCompensation);

    // Convert a variable index to a stack offset.
    constexpr int varToOffset(int var) const {
        return -var - 1;
    }

    // Returns the frame with ID 'frameId' if it already exists, else creates and returns it.
    FrameInfo& getOrDeclareFrame(FrameId frameId);

    // For a given 'frame' in this CodeFragment, subtracts the frame's 'stackPosition' from all the
    // refs to this frame in code (located at 'fixupOffset's). This is done once the true stack
    // position of the frame is known, so code refs point to the correct location in the frame.
    void fixupFrame(FrameInfo& frame);

    LabelInfo& getOrDeclareLabel(LabelId labelId);
    void fixupLabel(LabelInfo& label);

    // The sequence of byte code instructions this CodeFragment represents.
    absl::InlinedVector<uint8_t, 16> _instrs;

    // A collection of frame information for local variables.
    // Variables can be declared or referenced out of order and at the time of variable reference
    // it may not be known the relative stack offset of variable declaration w.r.t to its use.
    // This tracks both declaration info (stack depth) and use info (code offset).
    // When code is concatenated the offsets are adjusted if needed and when declaration stack depth
    // becomes known all fixups are resolved.
    absl::flat_hash_map<FrameId, FrameInfo> _frames;

    // A collection of label information for labels that are currently in scope.
    // Labels can be defined or referenced out of order and at at time of label reference (e.g:
    // jumps or lambda creation), the exact relative offset may not be yet known.
    // This tracks both label definition (code offset where label is defined) and use info for jumps
    // or lambdas (code offset). When code is concatenated the offsets are adjusted, if needed, and
    // when label definition offset becomes known all fixups are resolved.
    absl::flat_hash_map<LabelId, LabelInfo> _labels;

    // Delta number of '_argStack' entries effect of this CodeFragment; may be negative.
    int64_t _stackSize{0};

    // Maximum absolute number of entries in '_argStack' from this CodeFragment.
    int64_t _maxStackSize{0};
};

class ByteCode {
    // The number of bytes per stack entry.
    static constexpr size_t sizeOfElement =
        sizeof(bool) + sizeof(value::TypeTags) + sizeof(value::Value);
    static_assert(sizeOfElement == 10);
    static_assert(std::is_trivially_copyable_v<FastTuple<bool, value::TypeTags, value::Value>>);

public:
    struct InvokeLambdaFunctor;
    struct GetFromStackFunctor;

    ByteCode() {
        _argStack = reinterpret_cast<uint8_t*>(mongoMalloc(sizeOfElement * 4));
        _argStackEnd = _argStack + sizeOfElement * 4;
        _argStackTop = _argStack - sizeOfElement;
    }

    ~ByteCode() {
        std::free(_argStack);
    }

    ByteCode(const ByteCode&) = delete;
    ByteCode& operator=(const ByteCode&) = delete;

    FastTuple<bool, value::TypeTags, value::Value> run(const CodeFragment* code);
    bool runPredicate(const CodeFragment* code);

private:
    void runInternal(const CodeFragment* code, int64_t position);
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

    MONGO_COMPILER_ALWAYS_INLINE
    static FastTuple<bool, bool, int> decodeParam(const uint8_t*& pcPointer) noexcept {
        return Instruction::Parameter::decodeParam(pcPointer);
    }

    FastTuple<bool, value::TypeTags, value::Value> genericDiv(value::TypeTags lhsTag,
                                                              value::Value lhsValue,
                                                              value::TypeTags rhsTag,
                                                              value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericIDiv(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericMod(value::TypeTags lhsTag,
                                                              value::Value lhsValue,
                                                              value::TypeTags rhsTag,
                                                              value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAbs(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericCeil(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericFloor(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericExp(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericLn(value::TypeTags operandTag,
                                                             value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericLog10(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSqrt(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericPow(value::TypeTags baseTag,
                                                              value::Value baseValue,
                                                              value::TypeTags exponentTag,
                                                              value::Value exponentValue);
    FastTuple<bool, value::TypeTags, value::Value> genericRoundTrunc(
        std::string funcName,
        Decimal128::RoundingMode roundingMode,
        int32_t place,
        value::TypeTags numTag,
        value::Value numVal);
    FastTuple<bool, value::TypeTags, value::Value> scalarRoundTrunc(
        std::string funcName, Decimal128::RoundingMode roundingMode, ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> blockRoundTrunc(
        std::string funcName, Decimal128::RoundingMode roundingMode, ArityType arity);
    std::pair<value::TypeTags, value::Value> genericNot(value::TypeTags tag, value::Value value);

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
    void traverseP(const CodeFragment* code, int64_t position, int64_t maxDepth);
    void traverseP_nested(const CodeFragment* code,
                          int64_t position,
                          value::TypeTags tag,
                          value::Value val,
                          int64_t maxDepth);

    void traverseF(const CodeFragment* code);
    void traverseF(const CodeFragment* code, int64_t position, bool compareArray);
    void traverseFInArray(const CodeFragment* code, int64_t position, bool compareArray);
    void magicTraverseF(const CodeFragment* code);

    bool runLambdaPredicate(const CodeFragment* code, int64_t position);
    void traverseCsiCellValues(const CodeFragment* code, int64_t position);
    void traverseCsiCellTypes(const CodeFragment* code, int64_t position);
    void valueBlockApplyLambda(const CodeFragment* code);

    FastTuple<bool, value::TypeTags, value::Value> setField();

    int32_t convertNumericToInt32(value::TypeTags tag, value::Value val);

    FastTuple<bool, value::TypeTags, value::Value> getArraySize(value::TypeTags tag,
                                                                value::Value val);

    FastTuple<bool, value::TypeTags, value::Value> aggSum(value::TypeTags accTag,
                                                          value::Value accValue,
                                                          value::TypeTags fieldTag,
                                                          value::Value fieldValue);

    void aggDoubleDoubleSumImpl(value::Array* accumulator,
                                value::TypeTags rhsTag,
                                value::Value rhsValue);
    void aggMergeDoubleDoubleSumsImpl(value::Array* accumulator,
                                      value::TypeTags rhsTag,
                                      value::Value rhsValue);
    FastTuple<bool, value::TypeTags, value::Value> aggDoubleDoubleSumFinalizeImpl(
        value::Array* accmulator);

    // This is an implementation of the following algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
    void aggStdDevImpl(value::Array* accumulator, value::TypeTags rhsTag, value::Value rhsValue);
    void aggMergeStdDevsImpl(value::Array* accumulator,
                             value::TypeTags rhsTag,
                             value::Value rhsValue);

    FastTuple<bool, value::TypeTags, value::Value> aggStdDevFinalizeImpl(value::Value fieldValue,
                                                                         bool isSamp);

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

    FastTuple<bool, value::TypeTags, value::Value> genericAcos(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAcosh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAsin(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAsinh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtan(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtanh(value::TypeTags operandTag,
                                                                value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericAtan2(value::TypeTags operandTag1,
                                                                value::Value operandValue1,
                                                                value::TypeTags operandTag2,
                                                                value::Value operandValue2);
    FastTuple<bool, value::TypeTags, value::Value> genericCos(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericCosh(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericDegreesToRadians(
        value::TypeTags operandTag, value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericRadiansToDegrees(
        value::TypeTags operandTag, value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSin(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericSinh(value::TypeTags operandTag,
                                                               value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericTan(value::TypeTags operandTag,
                                                              value::Value operandValue);
    FastTuple<bool, value::TypeTags, value::Value> genericTanh(value::TypeTags operandTag,
                                                               value::Value operandValue);

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

    /**
     * produceBsonObject() takes a MakeObjSpec ('spec'), a root value ('rootTag' and 'rootVal'),
     * and 0 or more "computed" values as inputs, it builds an output BSON object based on the
     * instructions provided by 'spec' and based on the contents of 'root' and the computed input
     * values, and then it returns the output object. (Note the computed input values are not
     * directly passed in as C++ parameters -- instead the computed input values are passed via
     * the VM's stack.)
     */
    MONGO_COMPILER_ALWAYS_INLINE void produceBsonObject(const MakeObjSpec* spec,
                                                        MakeObjStackOffsets stackOffsets,
                                                        const CodeFragment* code,
                                                        UniqueBSONObjBuilder& bob,
                                                        value::TypeTags rootTag,
                                                        value::Value rootVal) {
        using TypeTags = value::TypeTags;

        const auto& fields = spec->fields;

        // Invoke the produceBsonObject() lambda with the appropriate iterator type. For
        // SBE objects, we use ObjectCursor. For all other types, we use BsonObjCursor.
        if (rootTag == TypeTags::Object) {
            auto obj = value::getObjectView(rootVal);

            produceBsonObject(spec, stackOffsets, code, bob, ObjectCursor(fields, obj));
        } else {
            const char* obj = rootTag == TypeTags::bsonObject
                ? value::bitcastTo<const char*>(rootVal)
                : BSONObj::kEmptyObject.objdata();

            produceBsonObject(spec, stackOffsets, code, bob, BsonObjCursor(fields, obj));
        }
    }

    void produceBsonObjectWithInputFields(const MakeObjSpec* spec,
                                          MakeObjStackOffsets stackOffsets,
                                          const CodeFragment* code,
                                          UniqueBSONObjBuilder& bob,
                                          value::TypeTags objTag,
                                          value::Value objVal);

    template <typename CursorT>
    void produceBsonObject(const MakeObjSpec* spec,
                           MakeObjStackOffsets stackOffsets,
                           const CodeFragment* code,
                           UniqueBSONObjBuilder& bob,
                           CursorT cursor);

    /**
     * This struct is used by traverseAndProduceBsonObj() to hold args that stay the same across
     * each level of recursion. Also, by making use of this struct, on most common platforms we
     * will be able to pass all of traverseAndProduceBsonObj()'s args via CPU registers (rather
     * than passing them via the native stack).
     */
    struct TraverseAndProduceBsonObjContext {
        const MakeObjSpec* spec;
        MakeObjStackOffsets stackOffsets;
        const CodeFragment* code;
    };

    void traverseAndProduceBsonObj(const TraverseAndProduceBsonObjContext& ctx,
                                   value::TypeTags tag,
                                   value::Value val,
                                   int64_t maxDepth,
                                   UniqueBSONArrayBuilder& bab);

    void traverseAndProduceBsonObj(const TraverseAndProduceBsonObjContext& ctx,
                                   value::TypeTags tag,
                                   value::Value val,
                                   StringData fieldName,
                                   UniqueBSONObjBuilder& bob);

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
    FastTuple<bool, value::TypeTags, value::Value> builtinAbs(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCeil(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinFloor(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTrunc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinExp(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinLn(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinLog10(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSqrt(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinPow(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToArrayCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMergeObjects(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSet(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> addToSetCappedImpl(value::TypeTags tagNewElem,
                                                                      value::Value valNewElem,
                                                                      int32_t sizeCap,
                                                                      CollatorInterface* collator);
    FastTuple<bool, value::TypeTags, value::Value> builtinAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCollAddToSetCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSetToArray(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConvertPartialCountSumToDoubleDoubleSum(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoubleSum(ArityType arity);
    // The template parameter is false for a regular DoubleDouble summation and true if merging
    // partially computed DoubleDouble sums.
    template <bool merging>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggDoubleDoubleSum(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoubleSumFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDoubleDoublePartialSumFinalize(
        ArityType arity);

    // The template parameter is false for a regular std dev and true if merging partially computed
    // standard devations.
    template <bool merging>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggStdDev(ArityType arity);

    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevPopFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStdDevSampFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestZero(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestMask(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBitTestPosition(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinBsonSize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinStrLenBytes(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToUpper(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinToLower(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCoerceToBool(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCoerceToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAcos(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAcosh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAsin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAsinh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtan(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtanh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAtan2(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCos(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCosh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDegreesToRadians(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRadiansToDegrees(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinSinh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTan(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTanh(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinRound(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcat(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinConcatArrays(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTrim(ArityType arity,
                                                               bool trimLeft,
                                                               bool trimRight);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggConcatArraysCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCollSetUnion(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggSetUnionCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCollSetUnionCapped(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> aggSetUnionCappedImpl(
        value::TypeTags tagNewElem,
        value::Value valNewElem,
        int32_t sizeCap,
        CollatorInterface* collator);
    FastTuple<bool, value::TypeTags, value::Value> builtinIsMember(ArityType arity);
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
    FastTuple<bool, value::TypeTags, value::Value> builtinMakeBsonObj(ArityType arity,
                                                                      const CodeFragment* code);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsSecond(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTsIncrement(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinTypeMatch(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateToString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateFromString(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateFromStringNoThrow(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinDateTrunc(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinMinMaxFromArray(ArityType arity,
                                                                          Builtin f);
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

    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstNNeedsMoreInput(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstNMerge(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstNFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLastN(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLastNMerge(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLastNFinalize(ArityType arity);
    template <typename Less>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggTopBottomN(ArityType arity);
    template <typename Less>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggTopBottomNMerge(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggTopBottomNFinalize(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggMinMaxN(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggMinMaxNMerge(ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggMinMaxNFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRank(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRankColl(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggDenseRank(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggDenseRankColl(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRankFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggExpMovingAvg(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggExpMovingAvgFinalize(ArityType arity);
    template <int sign>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableSum(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableSumFinalize(ArityType arity);
    template <int sign>
    void aggRemovableSumImpl(value::Array* state, value::TypeTags rhsTag, value::Value rhsVal);
    FastTuple<bool, value::TypeTags, value::Value> aggRemovableSumFinalizeImpl(value::Array* state);
    template <class T, int sign>
    void updateRemovableSumAccForIntegerType(value::Array* sumAcc,
                                             value::TypeTags rhsTag,
                                             value::Value rhsVal);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggIntegralInit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggIntegralAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggIntegralRemove(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggIntegralFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> integralOfTwoPointsByTrapezoidalRule(
        std::pair<value::TypeTags, value::Value> prevInput,
        std::pair<value::TypeTags, value::Value> prevSortByVal,
        std::pair<value::TypeTags, value::Value> newInput,
        std::pair<value::TypeTags, value::Value> newSortByVal);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggDerivativeFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> aggRemovableAvgFinalizeImpl(
        value::Array* sumState, int64_t count);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCovarianceAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCovarianceRemove(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCovarianceFinalize(ArityType arity,
                                                                                bool isSamp);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCovarianceSampFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggCovariancePopFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovablePushAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovablePushRemove(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovablePushFinalize(ArityType arity);
    template <int quantity>
    void aggRemovableStdDevImpl(value::TypeTags stateTag,
                                value::Value stateVal,
                                value::TypeTags inputTag,
                                value::Value inputVal);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableStdDevAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableStdDevRemove(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableStdDevFinalize(
        ArityType arity, bool isSamp);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableStdDevSampFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableStdDevPopFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAvgFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstLastNInit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstLastNAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstLastNRemove(ArityType arity);
    template <AccumulatorFirstLastN::Sense S>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggFirstLastNFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLinearFillCanAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLinearFillAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggLinearFillFinalize(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAddToSetInit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAddToSetCollInit(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAddToSetAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAddToSetRemove(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableAddToSetFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> aggRemovableMinMaxNInitImpl(
        CollatorInterface* collator);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableMinMaxNCollInit(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableMinMaxNInit(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableMinMaxNAdd(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableMinMaxNRemove(
        ArityType arity);
    template <AccumulatorMinMaxN::MinMaxSense S>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableMinMaxNFinalize(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> linearFillInterpolate(
        std::pair<value::TypeTags, value::Value> x1,
        std::pair<value::TypeTags, value::Value> y1,
        std::pair<value::TypeTags, value::Value> x2,
        std::pair<value::TypeTags, value::Value> y2,
        std::pair<value::TypeTags, value::Value> x);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableTopBottomNInit(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableTopBottomNAdd(
        ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableTopBottomNRemove(
        ArityType arity);
    template <TopBottomSense>
    FastTuple<bool, value::TypeTags, value::Value> builtinAggRemovableTopBottomNFinalize(
        ArityType arity);

    // Block builtins
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockExists(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockFillEmpty(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockFillEmptyBlock(ArityType arity);
    template <bool less>
    FastTuple<bool, value::TypeTags, value::Value> valueBlockAggMinMaxImpl(
        value::ValueBlock* inputBlock, value::ValueBlock* bitsetBlock);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggMin(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggMax(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggCount(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinValueBlockAggSum(ArityType arity);

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
        std::pair<value::TypeTags, value::Value> scalar,
        value::ValueBlock* block,
        size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinScalarBlockArithmeticOperation(
        std::pair<value::TypeTags, value::Value> scalar, value::ValueBlock* block, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockScalarArithmeticOperation(
        const value::TypeTags* bitsetTags,
        const value::Value* bitsetVals,
        value::ValueBlock* block,
        std::pair<value::TypeTags, value::Value> scalar,
        size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinBlockScalarArithmeticOperation(
        value::ValueBlock* block, std::pair<value::TypeTags, value::Value> scalar, size_t valsNum);
    template <int operation>
    FastTuple<bool, value::TypeTags, value::Value> builtinScalarScalarArithmeticOperation(
        std::pair<value::TypeTags, value::Value> leftInputScalar,
        std::pair<value::TypeTags, value::Value> rightInputScalar,
        size_t valsNum);
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
    FastTuple<bool, value::TypeTags, value::Value> builtinCellFoldValues_F(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCellFoldValues_P(ArityType arity);
    FastTuple<bool, value::TypeTags, value::Value> builtinCellBlockGetFlatValuesBlock(
        ArityType arity);

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
    FastTuple<bool, value::TypeTags, value::Value> getFromStack(size_t offset,
                                                                bool pop = false) noexcept {
        auto ret = readTuple(_argStackTop - offset * sizeOfElement);

        if (pop) {
            popStack();
        }

        return ret;
    }

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
    void pushStack(bool owned, value::TypeTags tag, value::Value val) noexcept {
        auto localPtr = _argStackTop += sizeOfElement;
        if constexpr (kDebugBuild) {
            invariant(localPtr != _argStackEnd);
        }

        writeTuple(localPtr, owned, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void topStack(bool owned,
                                               value::TypeTags tag,
                                               value::Value val) noexcept {
        writeTuple(_argStackTop, owned, tag, val);
    }

    MONGO_COMPILER_ALWAYS_INLINE void popStack() noexcept {
        _argStackTop -= sizeOfElement;
    }

    MONGO_COMPILER_ALWAYS_INLINE_OPT
    void popAndReleaseStack() noexcept {
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

struct ByteCode::GetFromStackFunctor {
    GetFromStackFunctor(ByteCode& bytecode, int stackStartOffset)
        : bytecode(&bytecode), stackStartOffset(stackStartOffset) {}

    FastTuple<bool, value::TypeTags, value::Value> operator()(size_t idx) const {
        return bytecode->getFromStack(stackStartOffset + idx);
    }

    ByteCode* bytecode;
    const int stackStartOffset;
};

class MakeObjCursorInputFields {
public:
    MakeObjCursorInputFields(ByteCode& bytecode, int startOffset, size_t numFields)
        : _getFieldFn(bytecode, startOffset), _numFields(numFields) {}

    size_t size() const {
        return _numFields;
    }

    FastTuple<bool, value::TypeTags, value::Value> operator[](size_t idx) const {
        return _getFieldFn(idx);
    }

private:
    ByteCode::GetFromStackFunctor _getFieldFn;
    size_t _numFields;
};

class InputFieldsOnlyCursor;
class BsonObjWithInputFieldsCursor;
class ObjWithInputFieldsCursor;

// There are five instantiations of the templated produceBsonObject() method, one for each
// type of MakeObj input cursor.
extern template void ByteCode::produceBsonObject<BsonObjCursor>(const MakeObjSpec* spec,
                                                                MakeObjStackOffsets stackOffsets,
                                                                const CodeFragment* code,
                                                                UniqueBSONObjBuilder& bob,
                                                                BsonObjCursor cursor);

extern template void ByteCode::produceBsonObject<ObjectCursor>(const MakeObjSpec* spec,
                                                               MakeObjStackOffsets stackOffsets,
                                                               const CodeFragment* code,
                                                               UniqueBSONObjBuilder& bob,
                                                               ObjectCursor cursor);

extern template void ByteCode::produceBsonObject<InputFieldsOnlyCursor>(
    const MakeObjSpec* spec,
    MakeObjStackOffsets stackOffsets,
    const CodeFragment* code,
    UniqueBSONObjBuilder& bob,
    InputFieldsOnlyCursor cursor);

extern template void ByteCode::produceBsonObject<BsonObjWithInputFieldsCursor>(
    const MakeObjSpec* spec,
    MakeObjStackOffsets stackOffsets,
    const CodeFragment* code,
    UniqueBSONObjBuilder& bob,
    BsonObjWithInputFieldsCursor cursor);

extern template void ByteCode::produceBsonObject<ObjWithInputFieldsCursor>(
    const MakeObjSpec* spec,
    MakeObjStackOffsets stackOffsets,
    const CodeFragment* code,
    UniqueBSONObjBuilder& bob,
    ObjWithInputFieldsCursor cursor);
}  // namespace vm
}  // namespace sbe
}  // namespace mongo
