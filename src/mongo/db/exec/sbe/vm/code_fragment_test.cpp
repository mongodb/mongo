// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/exec/sbe/vm/code_fragment.h"

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/sbe/vm/vm_instruction.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * These tests are for CodeFragment::appendAccessVal.  In particular, testing that the
 * values of the SlotAccessors being appended are passed through and deserialized correctly.
 */
TEST(CodeFragmentTest, TestPushEnvAccessorVal) {
    sbe::vm::CodeFragment fragment;
    sbe::RuntimeEnvironment env;
    sbe::value::TypeTags testTag = sbe::value::TypeTags::Boolean;
    sbe::value::Value testValue = 192;
    sbe::value::SlotIdGenerator incrementingGenerator;
    env.registerSlot(testTag, testValue, true, &incrementingGenerator);
    sbe::RuntimeEnvironment::Accessor accessor(&env, 0);
    fragment.appendAccessVal(&accessor);
    sbe::vm::ByteCode vm;
    auto res = vm.run(&fragment);
    ASSERT(!res.owned());
    ASSERT(res.tag() == testTag);
    ASSERT(res.value() == testValue);
}
TEST(CodeFragmentTest, TestPushOwnedAccessorVal) {
    sbe::vm::CodeFragment fragment;
    sbe::value::OwnedValueAccessor accessor;
    sbe::value::TypeTags testTag = sbe::value::TypeTags::Boolean;
    sbe::value::Value testValue = 192;
    accessor.reset(testTag, testValue);
    fragment.appendAccessVal(&accessor);
    sbe::vm::ByteCode vm;
    auto res = vm.run(&fragment);
    ASSERT(!res.owned());
    ASSERT(res.tag() == testTag);
    ASSERT(res.value() == testValue);
}

DEATH_TEST(SBEVMBytecodeValidationDeathTest,
           InvalidOpcodeTriggersUnreachable,
           "SBE lastInstruction VM opcode") {
    constexpr uint8_t kInvalidOpcode = static_cast<uint8_t>(sbe::vm::Instruction::lastInstruction);
    sbe::vm::CodeFragment code;
    code.instrs().resize(1, kInvalidOpcode);
    sbe::vm::ByteCode interpreter;
    interpreter.run(&code);
}

TEST(CodeFragmentTest, AggCollMinMalformedCollation) {
    auto [accTag, accVal] = sbe::value::makeNewString("accumulate me"sv);
    sbe::value::OwnedValueAccessor accAccessor;
    accAccessor.reset(accTag, accVal);

    sbe::vm::CodeFragment fragment;
    fragment.appendMoveVal(&accAccessor);
    fragment.appendConstVal(sbe::value::TypeTags::Nothing, sbe::value::Value{0});
    sbe::value::TagValueOwned field{sbe::value::makeNewString("fieldName"sv)};
    fragment.appendConstVal(field.tag(), field.value());
    fragment.appendCollMin();

    sbe::vm::ByteCode vm;
    auto res = vm.run(&fragment);

    ASSERT(res.owned());
    ASSERT(sbe::value::isString(res.tag()));
    ASSERT_EQ(sbe::value::getStringView(res.tag(), res.value()), "accumulate me"sv);
}

TEST(CodeFragmentTest, AggCollMaxMalformedCollation) {
    auto [accTag, accVal] = sbe::value::makeNewString("accumulate me"sv);
    sbe::value::OwnedValueAccessor accAccessor;
    accAccessor.reset(accTag, accVal);

    sbe::vm::CodeFragment fragment;
    fragment.appendMoveVal(&accAccessor);
    fragment.appendConstVal(sbe::value::TypeTags::Nothing, sbe::value::Value{0});
    sbe::value::TagValueOwned field{sbe::value::makeNewString("fieldName"sv)};
    fragment.appendConstVal(field.tag(), field.value());
    fragment.appendCollMax();

    sbe::vm::ByteCode vm;
    auto res = vm.run(&fragment);

    ASSERT(res.owned());
    ASSERT(sbe::value::isString(res.tag()));
    ASSERT_EQ(sbe::value::getStringView(res.tag(), res.value()), "accumulate me"sv);
}

DEATH_TEST(SBEVMBytecodeValidationDeathTest,
           JmpOobReadPastInstrsEnd,
           "SBE VM bytecode pointer should not exceed bytecode end") {
    sbe::vm::CodeFragment code;
    auto& instrs = code.instrs();
    constexpr size_t kJmpSize = sizeof(sbe::vm::Instruction) + sizeof(int);
    instrs.resize(kJmpSize, 0);
    sbe::vm::Instruction jmpInstr;
    jmpInstr.tag = sbe::vm::Instruction::jmp;
    sbe::vm::writeToMemory(instrs.data(), jmpInstr);
    int jumpOffset = 1;
    sbe::vm::writeToMemory(instrs.data() + sizeof(sbe::vm::Instruction), jumpOffset);
    sbe::vm::ByteCode interpreter;
    interpreter.run(&code);
}

}  // namespace mongo
