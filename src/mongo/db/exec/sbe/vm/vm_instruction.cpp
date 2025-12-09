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

#include "mongo/db/exec/sbe/vm/vm_instruction.h"

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/code_fragment.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collation_index_key.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#if defined(_MSC_VER)
#define USE_THREADED_INTERPRETER 0
#else
#define USE_THREADED_INTERPRETER 1
#endif

namespace mongo {
namespace sbe {
namespace vm {
namespace {
MONGO_COMPILER_NOINLINE
std::pair<value::TypeTags, value::Value> collComparisonKey(value::TypeTags tag,
                                                           value::Value val,
                                                           const CollatorInterface* collator) {
    using namespace std::literals;

    // This function should only be called if 'collator' is non-null and 'tag' is a collatable type.
    tassert(11054001, "Missing collator argument in collComparisonKey", collator);
    tassert(11086802, "Unexpected value of non-collatable type", value::isCollatableType(tag));

    // For strings, call CollatorInterface::getComparisonKey() to obtain the comparison key.
    if (value::isString(tag)) {
        return value::makeNewString(
            collator->getComparisonKey(value::getStringView(tag, val)).getKeyData());
    }

    // For collatable types other than strings (such as arrays and objects), we take the slow
    // path and round-trip the value through BSON.
    BSONObjBuilder input;
    bson::appendValueToBsonObj<BSONObjBuilder>(input, ""_sd, tag, val);

    BSONObjBuilder output;
    CollationIndexKey::collationAwareIndexKeyAppend(input.obj().firstElement(), collator, &output);

    BSONObj outputView = output.done();
    auto ptr = outputView.objdata();
    auto be = ptr + 4;
    auto end = ptr + ConstDataView(ptr).read<LittleEndian<uint32_t>>();
    return bson::convertFrom<false>(be, end, 0);
}
}  // namespace

/*
 * This table must be kept in sync with Instruction::Tags. It encodes how the instruction affects
 * the stack; i.e. push(+1), pop(-1), or no effect.
 */
int Instruction::stackOffset[Instruction::Tags::lastInstruction] = {
    1,   // pushConstVal
    1,   // pushAccessVal
    1,   // pushOwnedAccessorVal
    1,   // pushEnvAccessorVal
    1,   // pushMoveVal
    1,   // pushLocalVal
    1,   // pushMoveLocalVal
    1,   // pushOneArgLambda
    1,   // pushTwoArgLambda
    -1,  // pop
    0,   // swap
    0,   // makeOwn

    -1,  // add
    -1,  // sub
    -1,  // mul
    -1,  // div
    -1,  // idiv
    -1,  // mod
    0,   // negate
    0,   // numConvert

    0,  // logicNot

    -1,  // less
    -1,  // lessEq
    -1,  // greater
    -1,  // greaterEq
    -1,  // eq
    -1,  // neq
    -1,  // cmp3w

    -2,  // collLess
    -2,  // collLessEq
    -2,  // collGreater
    -2,  // collGreaterEq
    -2,  // collEq
    -2,  // collNeq
    -2,  // collCmp3w

    -1,  // fillEmpty
    0,   // fillEmptyImm
    -1,  // getField
    0,   // getFieldImm
    -1,  // getElement
    -1,  // collComparisonKey
    -1,  // getFieldOrElement
    -2,  // traverseP
    0,   // traversePImm
    -2,  // traverseF
    0,   // traverseFImm
    -4,  // magicTraverseF
    -2,  // setField
    0,   // getArraySize

    -1,  // aggSum
    -1,  // aggCount
    -1,  // aggMin
    -1,  // aggMax
    -1,  // aggFirst
    -1,  // aggLast

    -1,  // aggCollMin
    -1,  // aggCollMax

    0,  // exists
    0,  // isNull
    0,  // isObject
    0,  // isArray
    0,  // isInList
    0,  // isString
    0,  // isNumber
    0,  // isBinData
    0,  // isDate
    0,  // isNaN
    0,  // isInfinity
    0,  // isRecordId
    0,  // isMinKey
    0,  // isMaxKey
    0,  // isTimestamp
    0,  // isKeyString
    0,  // typeMatchImm

    0,  // function is special, the stack offset is encoded in the instruction itself
    0,  // functionSmall is special, the stack offset is encoded in the instruction itself

    0,   // jmp
    -1,  // jmpTrue
    -1,  // jmpFalse
    0,   // jmpNothing
    0,   // jmpNotNothing
    0,   // ret
    0,   // allocStack does not affect the top of stack

    -1,  // fail

    0,  // dateTruncImm

    -1,  // valueBlockApplyLambda
};  // int Instruction::stackOffset


MONGO_COMPILER_NORETURN void reportSwapFailure();

void ByteCode::swapStack() {
    auto [rhsOwned, rhsTag, rhsValue] = getFromStack(0);
    auto [lhsOwned, lhsTag, lhsValue] = getFromStack(1);

    // Swap values only if they are not physically same. This is necessary for the
    // "swap and pop" idiom for returning a value from the top of the stack (used
    // by ELocalBind). For example, consider the case where a series of swap, pop,
    // swap, pop... instructions are executed and the value at stack[0] and
    // stack[1] are physically identical, but stack[1] is owned and stack[0] is
    // not. After swapping them, the 'pop' instruction would free the owned one and
    // leave the unowned value dangling. The only exception to this is shallow
    // values (values which fit directly inside a 64 bit Value and don't need
    // to be freed explicitly).
    if (rhsValue == lhsValue && rhsTag == lhsTag) {
        if (rhsOwned && !isShallowType(rhsTag)) {
            reportSwapFailure();
        }
    } else {
        setStack(0, lhsOwned, lhsTag, lhsValue);
        setStack(1, rhsOwned, rhsTag, rhsValue);
    }
}

MONGO_COMPILER_NORETURN void reportSwapFailure() {
    tasserted(56123, "Attempting to swap two identical values when top of stack is owned");
}

MONGO_COMPILER_NORETURN void ByteCode::runFailInstruction() {
    auto [ownedCode, tagCode, valCode] = getFromStack(1);
    tassert(11086801, "Unexpected error code type", tagCode == value::TypeTags::NumberInt64);

    auto [ownedMsg, tagMsg, valMsg] = getFromStack(0);
    tassert(11086800, "Unexpected error message type", value::isString(tagMsg));

    ErrorCodes::Error code{static_cast<ErrorCodes::Error>(value::bitcastTo<int64_t>(valCode))};
    std::string message{value::getStringView(tagMsg, valMsg)};

    uasserted(code, message);
}

template <typename T>
void ByteCode::runTagCheck(const uint8_t*& pcPointer, T&& predicate) {
    auto [popParam, moveFromParam, offsetParam] = Instruction::Parameter::decodeParam(pcPointer);
    auto [owned, tag, val] = getFromStack(offsetParam, popParam);

    if (tag != value::TypeTags::Nothing) {
        pushStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(predicate(tag)));
    } else {
        pushStack(false, value::TypeTags::Nothing, 0);
    }

    if (owned && popParam) {
        value::releaseValue(tag, val);
    }
}

void ByteCode::runTagCheck(const uint8_t*& pcPointer, value::TypeTags tagRhs) {
    runTagCheck(pcPointer, [tagRhs](value::TypeTags tagLhs) { return tagLhs == tagRhs; });
}

void ByteCode::runLambdaInternal(const CodeFragment* code, int64_t position) {
    runInternal(code, position);
    swapStack();
    popAndReleaseStack();
}

void ByteCode::runInternal(const CodeFragment* code, int64_t position) {
#if USE_THREADED_INTERPRETER
    // Very important this in sync with Instruction::Tags.
    static constexpr void* dispatchTable[std::numeric_limits<decltype(Instruction::tag)>::max() +
                                         1] = {&&do_pushConstVal,
                                               &&do_pushAccessVal,
                                               &&do_pushOwnedAccessorVal,
                                               &&do_pushEnvAccessorVal,
                                               &&do_pushMoveVal,
                                               &&do_pushLocalVal,
                                               &&do_pushMoveLocalVal,
                                               &&do_pushOneArgLambda,
                                               &&do_pushTwoArgLambda,
                                               &&do_pop,
                                               &&do_swap,
                                               &&do_makeOwn,

                                               &&do_add,
                                               &&do_sub,
                                               &&do_mul,
                                               &&do_div,
                                               &&do_idiv,
                                               &&do_mod,
                                               &&do_negate,
                                               &&do_numConvert,

                                               &&do_logicNot,

                                               &&do_less,
                                               &&do_lessEq,
                                               &&do_greater,
                                               &&do_greaterEq,
                                               &&do_eq,
                                               &&do_neq,

                                               &&do_cmp3w,

                                               &&do_collLess,
                                               &&do_collLessEq,
                                               &&do_collGreater,
                                               &&do_collGreaterEq,
                                               &&do_collEq,
                                               &&do_collNeq,
                                               &&do_collCmp3w,

                                               &&do_fillEmpty,
                                               &&do_fillEmptyImm,
                                               &&do_getField,
                                               &&do_getFieldImm,
                                               &&do_getElement,
                                               &&do_collComparisonKey,
                                               &&do_getFieldOrElement,
                                               &&do_traverseP,
                                               &&do_traversePImm,
                                               &&do_traverseF,
                                               &&do_traverseFImm,
                                               &&do_magicTraverseF,
                                               &&do_setField,
                                               &&do_getArraySize,

                                               &&do_aggSum,
                                               &&do_aggCount,
                                               &&do_aggMin,
                                               &&do_aggMax,
                                               &&do_aggFirst,
                                               &&do_aggLast,

                                               &&do_aggCollMin,
                                               &&do_aggCollMax,

                                               &&do_exists,
                                               &&do_isNull,
                                               &&do_isObject,
                                               &&do_isArray,
                                               &&do_isInList,
                                               &&do_isString,
                                               &&do_isNumber,
                                               &&do_isBinData,
                                               &&do_isDate,
                                               &&do_isNaN,
                                               &&do_isInfinity,
                                               &&do_isRecordId,
                                               &&do_isMinKey,
                                               &&do_isMaxKey,
                                               &&do_isTimestamp,
                                               &&do_isKeyString,
                                               &&do_typeMatchImm,

                                               &&do_function,
                                               &&do_functionSmall,

                                               &&do_jmp,
                                               &&do_jmpTrue,
                                               &&do_jmpFalse,
                                               &&do_jmpNothing,
                                               &&do_jmpNotNothing,
                                               &&do_ret,
                                               &&do_allocStack,

                                               &&do_fail,

                                               &&do_dateTruncImm,

                                               &&do_valueBlockApplyLambda};
#endif

    auto pcPointer = code->instrs().data() + position;
    auto pcEnd = pcPointer + code->instrs().size();

    /*
     * When support is available for the computed goto extension, use it to execute SBE bytecode
     * with the "threaded code" pattern. Otherwise, fall back to dispatching instructions
     * with a switch statement.
     *
     * Compared to using a switch statement in a loop, the threaded approach requires fewer
     * branches per instruction and executes slightly faster.
     */
#if USE_THREADED_INTERPRETER
    Instruction i;
#define INSTRUCTION(name) do_##name:
#define DISPATCH()                              \
    if (pcPointer == pcEnd) {                   \
        return;                                 \
    }                                           \
    i = readFromMemory<Instruction>(pcPointer); \
    pcPointer += sizeof(i);                     \
    goto* dispatchTable[i.tag]
    DISPATCH();
#else
#define INSTRUCTION(name) case Instruction::name:
#define DISPATCH() break
    while (pcPointer != pcEnd) {
        Instruction i = readFromMemory<Instruction>(pcPointer);
        pcPointer += sizeof(i);
        switch (i.tag) {
#endif

    // If we had a 'ret' instruction at the end of every code fragment, we could avoid the
    // 'pcPointer == pcEnd' conditional for each bytecode.
    INSTRUCTION(pushConstVal) {
        auto tag = readFromMemory<value::TypeTags>(pcPointer);
        pcPointer += sizeof(tag);
        auto val = readFromMemory<value::Value>(pcPointer);
        pcPointer += sizeof(val);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushAccessVal) {
        auto accessor = readFromMemory<value::SlotAccessor*>(pcPointer);
        pcPointer += sizeof(accessor);

        auto [tag, val] = accessor->getViewOfValue();
        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushOwnedAccessorVal) {
        auto accessor = readFromMemory<value::OwnedValueAccessor*>(pcPointer);
        pcPointer += sizeof(accessor);

        auto [tag, val] = accessor->getViewOfValue();
        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushEnvAccessorVal) {
        auto accessor = readFromMemory<RuntimeEnvironment::Accessor*>(pcPointer);
        pcPointer += sizeof(accessor);

        auto [tag, val] = accessor->getViewOfValue();
        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushMoveVal) {
        auto accessor = readFromMemory<value::SlotAccessor*>(pcPointer);
        pcPointer += sizeof(accessor);

        auto [tag, val] = accessor->copyOrMoveValue().releaseToRaw();
        pushStack(true, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushLocalVal) {
        auto stackOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(stackOffset);

        auto [owned, tag, val] = getFromStack(stackOffset);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushMoveLocalVal) {
        auto stackOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(stackOffset);

        auto [owned, tag, val] = getFromStack(stackOffset);
        setTagToNothing(stackOffset);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(pushOneArgLambda) {
        auto offset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(offset);
        auto newPosition = pcPointer - code->instrs().data() + offset;
        pushStack(
            false, value::TypeTags::LocalOneArgLambda, value::bitcastFrom<int64_t>(newPosition));
    }
    DISPATCH();
    INSTRUCTION(pushTwoArgLambda) {
        auto offset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(offset);
        auto newPosition = pcPointer - code->instrs().data() + offset;
        pushStack(
            false, value::TypeTags::LocalTwoArgLambda, value::bitcastFrom<int64_t>(newPosition));
    }
    DISPATCH();
    INSTRUCTION(pop) {
        popAndReleaseStack();
    }
    DISPATCH();
    INSTRUCTION(swap) {
        swapStack();
    }
    DISPATCH();
    INSTRUCTION(makeOwn) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = moveFromStack(offsetParam);
        if (!owned) {
            // Value is not owned, make a copy.
            std::tie(tag, val) = value::copyValue(tag, val);
        }
        // Pop the stack entry if required, otherwise invalidate it to avoid the error
        // "swapping stack entries containing the same value".
        if (popParam) {
            popStack();
        } else {
            setTagToNothing(offsetParam);
        }
        pushStack(true, tag, val);
    }
    DISPATCH();
    INSTRUCTION(add) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericAdd(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(sub) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericSub(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(mul) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericMul(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(div) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericDiv(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(idiv) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericIDiv(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(mod) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = genericMod(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(negate) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);
        value::ValueGuard paramGuard(owned && popParam, tag, val);

        auto [resultOwned, resultTag, resultVal] =
            genericSub(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0), tag, val);

        pushStack(resultOwned, resultTag, resultVal);
    }
    DISPATCH();
    INSTRUCTION(numConvert) {
        auto tag = readFromMemory<value::TypeTags>(pcPointer);
        pcPointer += sizeof(tag);

        auto [owned, lhsTag, lhsVal] = getFromStack(0);

        auto [rhsOwned, rhsTag, rhsVal] = genericNumConvert(lhsTag, lhsVal, tag);

        topStack(rhsOwned, rhsTag, rhsVal);

        if (owned) {
            value::releaseValue(lhsTag, lhsVal);
        }
    }
    DISPATCH();
    INSTRUCTION(logicNot) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);
        value::ValueGuard paramGuard(owned && popParam, tag, val);

        auto [resultTag, resultVal] = genericNot(tag, val);

        pushStack(false, resultTag, resultVal);
    }
    DISPATCH();
    INSTRUCTION(less) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericLt(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collLess) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericLt(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(lessEq) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericLte(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collLessEq) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericLte(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(greater) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericGt(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collGreater) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericGt(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(greaterEq) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericGte(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collGreaterEq) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericGte(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(eq) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericEq(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collEq) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericEq(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(neq) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::genericNeq(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collNeq) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::genericNeq(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(cmp3w) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [tag, val] = value::compare3way(lhsTag, lhsVal, rhsTag, rhsVal);

        pushStack(false, tag, val);
    }
    DISPATCH();
    INSTRUCTION(collCmp3w) {
        auto [popColl, moveFromColl, offsetColl] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);
        auto [collOwned, collTag, collVal] = getFromStack(offsetColl, popColl);
        value::ValueGuard collGuard(collOwned && popColl, collTag, collVal);

        if (collTag == value::TypeTags::collator) {
            auto comp = static_cast<StringDataComparator*>(value::getCollatorView(collVal));
            auto [tag, val] = value::compare3way(lhsTag, lhsVal, rhsTag, rhsVal, comp);
            pushStack(false, tag, val);
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(fillEmpty) {
        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(0);
        popStack();
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);

        if (lhsTag == value::TypeTags::Nothing) {
            topStack(rhsOwned, rhsTag, rhsVal);

            if (lhsOwned) {
                value::releaseValue(lhsTag, lhsVal);
            }
        } else {
            if (rhsOwned) {
                value::releaseValue(rhsTag, rhsVal);
            }
        }
    }
    DISPATCH();
    INSTRUCTION(fillEmptyImm) {
        auto k = readFromMemory<Instruction::Constants>(pcPointer);
        pcPointer += sizeof(k);

        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(0);
        if (lhsTag == value::TypeTags::Nothing) {
            switch (k) {
                case Instruction::Nothing:
                    break;
                case Instruction::Null:
                    topStack(false, value::TypeTags::Null, 0);
                    break;
                case Instruction::True:
                    topStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
                    break;
                case Instruction::False:
                    topStack(false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
                    break;
                case Instruction::Int32One:
                    topStack(false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
                    break;
                default:
                    MONGO_UNREACHABLE_TASSERT(11122949);
            }
        }
        DISPATCH();
    }
    INSTRUCTION(getField) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);

        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = getField(lhsTag, lhsVal, rhsTag, rhsVal);

        // Copy value only if needed
        if (lhsOwned && !owned) {
            owned = true;
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(getFieldImm) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto size = readFromMemory<uint8_t>(pcPointer);
        pcPointer += sizeof(size);
        StringData fieldName(reinterpret_cast<const char*>(pcPointer), size);
        pcPointer += size;

        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = getField(lhsTag, lhsVal, fieldName);

        // Copy value only if needed
        if (lhsOwned && !owned) {
            owned = true;
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(getElement) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = getElement(lhsTag, lhsVal, rhsTag, rhsVal);

        // Copy value only if needed
        if (lhsOwned && !owned) {
            owned = true;
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(getArraySize) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);
        value::ValueGuard paramGuard(owned && popParam, tag, val);

        auto [resultOwned, resultTag, resultVal] = getArraySize(tag, val);
        pushStack(resultOwned, resultTag, resultVal);
    }
    DISPATCH();
    INSTRUCTION(collComparisonKey) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        if (lhsTag != value::TypeTags::Nothing && rhsTag == value::TypeTags::collator) {
            // If lhs is a collatable type, call collComparisonKey() to obtain the
            // comparison key. If lhs is not a collatable type, we can just leave it
            // on the stack as-is.
            if (value::isCollatableType(lhsTag)) {
                auto collator = value::getCollatorView(rhsVal);
                auto [tag, val] = collComparisonKey(lhsTag, lhsVal, collator);
                pushStack(true, tag, val);
            } else {
                if (popLhs) {
                    pushStack(lhsOwned, lhsTag, lhsVal);
                    lhsGuard.reset();
                } else if (moveFromLhs) {
                    setTagToNothing(offsetLhs);
                    pushStack(lhsOwned, lhsTag, lhsVal);
                } else {
                    pushStack(false, lhsTag, lhsVal);
                }
            }
        } else {
            // If lhs was Nothing or rhs wasn't Collator, return Nothing.
            pushStack(false, value::TypeTags::Nothing, 0);
        }
    }
    DISPATCH();
    INSTRUCTION(getFieldOrElement) {
        auto [popLhs, moveFromLhs, offsetLhs] = Instruction::Parameter::decodeParam(pcPointer);
        auto [popRhs, moveFromRhs, offsetRhs] = Instruction::Parameter::decodeParam(pcPointer);

        auto [rhsOwned, rhsTag, rhsVal] = getFromStack(offsetRhs, popRhs);
        value::ValueGuard rhsGuard(rhsOwned && popRhs, rhsTag, rhsVal);
        auto [lhsOwned, lhsTag, lhsVal] = getFromStack(offsetLhs, popLhs);
        value::ValueGuard lhsGuard(lhsOwned && popLhs, lhsTag, lhsVal);

        auto [owned, tag, val] = getFieldOrElement(lhsTag, lhsVal, rhsTag, rhsVal);

        // Copy value only if needed
        if (lhsOwned && !owned) {
            owned = true;
            std::tie(tag, val) = value::copyValue(tag, val);
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(traverseP) {
        traverseP(code);
    }
    DISPATCH();
    INSTRUCTION(traversePImm) {
        auto providePosition = readFromMemory<Instruction::Constants>(pcPointer);
        pcPointer += sizeof(providePosition);
        auto k = readFromMemory<Instruction::Constants>(pcPointer);
        pcPointer += sizeof(k);

        auto offset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(offset);
        auto codePosition = pcPointer - code->instrs().data() + offset;

        traverseP(code,
                  codePosition,
                  providePosition == Instruction::True ? true : false,
                  k == Instruction::Nothing ? std::numeric_limits<int64_t>::max() : 1);
    }
    DISPATCH();
    INSTRUCTION(traverseF) {
        traverseF(code);
    }
    DISPATCH();
    INSTRUCTION(traverseFImm) {
        auto providePosition = readFromMemory<Instruction::Constants>(pcPointer);
        pcPointer += sizeof(providePosition);
        auto k = readFromMemory<Instruction::Constants>(pcPointer);
        pcPointer += sizeof(k);

        auto offset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(offset);
        auto codePosition = pcPointer - code->instrs().data() + offset;

        traverseF(code,
                  codePosition,
                  providePosition == Instruction::True ? true : false,
                  k == Instruction::True ? true : false);
    }
    DISPATCH();
    INSTRUCTION(magicTraverseF) {
        magicTraverseF(code);
    }
    DISPATCH();
    INSTRUCTION(setField) {
        auto [owned, tag, val] = setField();
        popAndReleaseStack();
        popAndReleaseStack();
        popAndReleaseStack();

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(aggSum) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [accTag, accVal] = moveOwnedFromStack(0);

        auto [owned, tag, val] = aggSum(accTag, accVal, fieldTag, fieldVal);

        topStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(aggCount) {
        auto [accTag, accVal] = moveOwnedFromStack(0);

        auto [owned, tag, val] = aggCount(accTag, accVal);

        topStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(aggMin) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        auto [owned, tag, val] = aggMin(accTag, accVal, fieldTag, fieldVal);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(aggCollMin) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [collOwned, collTag, collVal] = getFromStack(0);
        value::ValueGuard collGuard(collOwned, collTag, collVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        // Skip aggregation step if the collation is Nothing or an unexpected type.
        if (collTag != value::TypeTags::collator) {
            auto [tag, val] = value::copyValue(accTag, accVal);
            topStack(true, tag, val);
            return;
        }
        auto collator = value::getCollatorView(collVal);

        auto [owned, tag, val] = aggMin(accTag, accVal, fieldTag, fieldVal, collator);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(aggMax) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        auto [owned, tag, val] = aggMax(accTag, accVal, fieldTag, fieldVal);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(aggCollMax) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [collOwned, collTag, collVal] = getFromStack(0);
        value::ValueGuard collGuard(collOwned, collTag, collVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        // Skip aggregation step if the collation is Nothing or an unexpected type.
        if (collTag != value::TypeTags::collator) {
            auto [tag, val] = value::copyValue(accTag, accVal);
            topStack(true, tag, val);
            return;
        }
        auto collator = value::getCollatorView(collVal);

        auto [owned, tag, val] = aggMax(accTag, accVal, fieldTag, fieldVal, collator);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(aggFirst) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        auto [owned, tag, val] = aggFirst(accTag, accVal, fieldTag, fieldVal);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(aggLast) {
        auto [fieldOwned, fieldTag, fieldVal] = getFromStack(0);
        value::ValueGuard fieldGuard(fieldOwned, fieldTag, fieldVal);
        popStack();

        auto [accOwned, accTag, accVal] = getFromStack(0);

        auto [owned, tag, val] = aggLast(accTag, accVal, fieldTag, fieldVal);

        topStack(owned, tag, val);
        if (accOwned) {
            value::releaseValue(accTag, accVal);
        }
    }
    DISPATCH();
    INSTRUCTION(exists) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);

        pushStack(false,
                  value::TypeTags::Boolean,
                  value::bitcastFrom<bool>(tag != value::TypeTags::Nothing));

        if (owned && popParam) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(isNull) {
        runTagCheck(pcPointer, value::TypeTags::Null);
    }
    DISPATCH();
    INSTRUCTION(isObject) {
        runTagCheck(pcPointer, value::isObject);
    }
    DISPATCH();
    INSTRUCTION(isArray) {
        runTagCheck(pcPointer, value::isArray);
    }
    DISPATCH();
    INSTRUCTION(isInList) {
        runTagCheck(pcPointer, value::isInList);
    }
    DISPATCH();
    INSTRUCTION(isString) {
        runTagCheck(pcPointer, value::isString);
    }
    DISPATCH();
    INSTRUCTION(isNumber) {
        runTagCheck(pcPointer, value::isNumber);
    }
    DISPATCH();
    INSTRUCTION(isBinData) {
        runTagCheck(pcPointer, value::isBinData);
    }
    DISPATCH();
    INSTRUCTION(isDate) {
        runTagCheck(pcPointer, value::TypeTags::Date);
    }
    DISPATCH();
    INSTRUCTION(isNaN) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);

        if (tag != value::TypeTags::Nothing) {
            pushStack(
                false, value::TypeTags::Boolean, value::bitcastFrom<bool>(value::isNaN(tag, val)));
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }

        if (owned && popParam) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(isInfinity) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto [owned, tag, val] = getFromStack(offsetParam, popParam);

        if (tag != value::TypeTags::Nothing) {
            pushStack(false,
                      value::TypeTags::Boolean,
                      value::bitcastFrom<bool>(value::isInfinity(tag, val)));
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
        if (owned && popParam) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(isRecordId) {
        runTagCheck(pcPointer, value::isRecordId);
    }
    DISPATCH();
    INSTRUCTION(isMinKey) {
        runTagCheck(pcPointer, value::TypeTags::MinKey);
    }
    DISPATCH();
    INSTRUCTION(isMaxKey) {
        runTagCheck(pcPointer, value::TypeTags::MaxKey);
    }
    DISPATCH();
    INSTRUCTION(isTimestamp) {
        runTagCheck(pcPointer, value::TypeTags::Timestamp);
    }
    DISPATCH();
    INSTRUCTION(isKeyString) {
        runTagCheck(pcPointer, value::TypeTags::keyString);
    }
    DISPATCH();
    INSTRUCTION(typeMatchImm) {
        auto [popParam, moveFromParam, offsetParam] =
            Instruction::Parameter::decodeParam(pcPointer);
        auto mask = readFromMemory<uint32_t>(pcPointer);
        pcPointer += sizeof(mask);

        auto [owned, tag, val] = getFromStack(offsetParam, popParam);

        if (tag != value::TypeTags::Nothing) {
            pushStack(false,
                      value::TypeTags::Boolean,
                      value::bitcastFrom<bool>(getBSONTypeMask(tag) & mask));
        } else {
            pushStack(false, value::TypeTags::Nothing, 0);
        }
        if (owned && popParam) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(functionSmall) {
        auto f = readFromMemory<SmallBuiltinType>(pcPointer);
        pcPointer += sizeof(f);
        SmallArityType arity{0};
        arity = readFromMemory<SmallArityType>(pcPointer);
        pcPointer += sizeof(SmallArityType);

        auto [owned, tag, val] = dispatchBuiltin(static_cast<Builtin>(f), arity, code);

        for (ArityType cnt = 0; cnt < arity; ++cnt) {
            popAndReleaseStack();
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(function) {
        auto f = readFromMemory<Builtin>(pcPointer);
        pcPointer += sizeof(f);
        ArityType arity{0};
        arity = readFromMemory<ArityType>(pcPointer);
        pcPointer += sizeof(ArityType);

        auto [owned, tag, val] = dispatchBuiltin(f, arity, code);

        for (ArityType cnt = 0; cnt < arity; ++cnt) {
            popAndReleaseStack();
        }

        pushStack(owned, tag, val);
    }
    DISPATCH();
    INSTRUCTION(jmp) {
        auto jumpOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(jumpOffset);

        pcPointer += jumpOffset;
    }
    DISPATCH();
    INSTRUCTION(jmpTrue) {
        auto jumpOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(jumpOffset);

        auto [owned, tag, val] = getFromStack(0);
        popStack();

        if (tag == value::TypeTags::Boolean && value::bitcastTo<bool>(val)) {
            pcPointer += jumpOffset;
        }

        if (owned) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(jmpFalse) {
        auto jumpOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(jumpOffset);

        auto [owned, tag, val] = getFromStack(0);
        popStack();

        if (tag == value::TypeTags::Boolean && !value::bitcastTo<bool>(val)) {
            pcPointer += jumpOffset;
        }

        if (owned) {
            value::releaseValue(tag, val);
        }
    }
    DISPATCH();
    INSTRUCTION(jmpNothing) {
        auto jumpOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(jumpOffset);

        auto [owned, tag, val] = getFromStack(0);
        if (tag == value::TypeTags::Nothing) {
            pcPointer += jumpOffset;
        }
    }
    DISPATCH();
    INSTRUCTION(jmpNotNothing) {
        auto jumpOffset = readFromMemory<int>(pcPointer);
        pcPointer += sizeof(jumpOffset);

        auto [owned, tag, val] = getFromStack(0);
        if (tag != value::TypeTags::Nothing) {
            pcPointer += jumpOffset;
        }
    }
    DISPATCH();
    INSTRUCTION(ret) {
        pcPointer = pcEnd;
        return;
    }
    INSTRUCTION(allocStack) {
        auto size = readFromMemory<uint32_t>(pcPointer);
        pcPointer += sizeof(size);

        allocStack(size);
    }
    DISPATCH();
    INSTRUCTION(fail) {
        runFailInstruction();
    }
    DISPATCH();
    INSTRUCTION(dateTruncImm) {
        auto unit = readFromMemory<TimeUnit>(pcPointer);
        pcPointer += sizeof(unit);
        auto binSize = readFromMemory<int64_t>(pcPointer);
        pcPointer += sizeof(binSize);
        auto timezone = readFromMemory<TimeZone>(pcPointer);
        pcPointer += sizeof(timezone);
        auto startOfWeek = readFromMemory<DayOfWeek>(pcPointer);
        pcPointer += sizeof(startOfWeek);

        auto [dateOwned, dateTag, dateVal] = getFromStack(0);

        auto [owned, tag, val] = dateTrunc(dateTag, dateVal, unit, binSize, timezone, startOfWeek);

        topStack(owned, tag, val);

        if (dateOwned) {
            value::releaseValue(dateTag, dateVal);
        }
    }
    DISPATCH();
    INSTRUCTION(valueBlockApplyLambda) {
        valueBlockApplyLambda(code);
    }
    DISPATCH();
#if USE_THREADED_INTERPRETER
#else
        }
    }
#endif
#undef DISPATCH
#undef INSTRUCTION
}  // ByteCode::runInternal

value::TagValueMaybeOwned ByteCode::run(const CodeFragment* code) {
    try {
        uassert(6040900,
                "The evaluation stack must be empty",
                _argStackTop + sizeOfElement == _argStack);

        allocStack(code->maxStackSize());
        runInternal(code, 0);

        uassert(4822801,
                "The evaluation stack must hold only a single value",
                _argStackTop == _argStack);

        // Transfer ownership of tag/val to the caller
        stackReset();

        return value::TagValueMaybeOwned::fromRaw(readTuple(_argStack));
    } catch (...) {
        auto sentinel = _argStack - sizeOfElement;
        while (_argStackTop != sentinel) {
            popAndReleaseStack();
        }
        throw;
    }
}

bool ByteCode::runPredicate(const CodeFragment* code) {
    value::TagValueMaybeOwned result = run(code);

    return (result.tag() == value::TypeTags::Boolean) && value::bitcastTo<bool>(result.value());
}

const char* Instruction::toStringConstants(Constants k) {
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

const char* Instruction::toString() const {
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
        case pushOneArgLambda:
            return "pushOneArgLambda";
        case pushTwoArgLambda:
            return "pushTwoArgLambda";
        case pop:
            return "pop";
        case swap:
            return "swap";
        case makeOwn:
            return "makeOwn";
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
        case setField:
            return "setField";
        case getArraySize:
            return "getArraySize";
        case aggSum:
            return "aggSum";
        case aggCount:
            return "aggCount";
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
        case isInList:
            return "isInList";
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
        case isKeyString:
            return "isKeyString";
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
}  // Instruction::toString

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
