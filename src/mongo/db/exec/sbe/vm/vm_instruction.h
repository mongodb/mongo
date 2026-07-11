// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm_memory.h"
#include "mongo/util/modules.h"

#if !defined(MONGO_CONFIG_DEBUG_BUILD)
#define MONGO_COMPILER_ALWAYS_INLINE_OPT MONGO_COMPILER_ALWAYS_INLINE
#else
#define MONGO_COMPILER_ALWAYS_INLINE_OPT
#endif

namespace mongo {
namespace sbe {
namespace vm {
/**
 * Enumeration of built-in VM instructions. These are dispatched by vm_instruction.cpp
 * ByteCode::runInternal().
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
        pushOneArgLambda,
        pushTwoArgLambda,
        pop,
        swapAndPop,
        makeOwn,
        // If the only argument is a stack-owned value, it is propagated unchanged;
        // if it is not owned by the stack, it returns a copy of it.

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
        getArraySize,  // number of elements

        aggSum,
        aggCount,
        aggMin,
        aggMax,
        aggFirst,
        aggLast,

        aggCollMin,
        aggCollMax,

        exists,
        isNull,
        isNullish,
        isObject,
        isArray,
        isInList,
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
        isKeyString,
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
        static FastTuple<bool, bool, int> decodeParam(const uint8_t*& pcPointer) {
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
    };  // struct Parameter

    /**
     * Converts an Instruction::Constants value to its name.
     */
    static const char* toStringConstants(Constants k);

    /**
     * Converts an Instruction::Tags value to its instruction name.
     */
    const char* toString() const;

    // Make sure that values in this arrays are always in-sync with the enum.
    static int stackOffset[];

    uint8_t tag;
};  // struct Instruction
static_assert(sizeof(Instruction) == sizeof(uint8_t));

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
