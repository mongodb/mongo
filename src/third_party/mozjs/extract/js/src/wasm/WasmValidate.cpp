/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmValidate.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/Utf8.h"

#include "jit/JitOptions.h"
#include "js/Printf.h"
#include "js/String.h"  // JS::MaxStringLength
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "wasm/WasmOpIter.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::AsChars;
using mozilla::CheckedInt;
using mozilla::CheckedInt32;
using mozilla::IsUtf8;
using mozilla::Span;

// Misc helpers.

bool wasm::EncodeLocalEntries(Encoder& e, const ValTypeVector& locals) {
  if (locals.length() > MaxLocals) {
    return false;
  }

  uint32_t numLocalEntries = 0;
  if (locals.length()) {
    ValType prev = locals[0];
    numLocalEntries++;
    for (ValType t : locals) {
      if (t != prev) {
        numLocalEntries++;
        prev = t;
      }
    }
  }

  if (!e.writeVarU32(numLocalEntries)) {
    return false;
  }

  if (numLocalEntries) {
    ValType prev = locals[0];
    uint32_t count = 1;
    for (uint32_t i = 1; i < locals.length(); i++, count++) {
      if (prev != locals[i]) {
        if (!e.writeVarU32(count)) {
          return false;
        }
        if (!e.writeValType(prev)) {
          return false;
        }
        prev = locals[i];
        count = 0;
      }
    }
    if (!e.writeVarU32(count)) {
      return false;
    }
    if (!e.writeValType(prev)) {
      return false;
    }
  }

  return true;
}

bool wasm::DecodeLocalEntries(Decoder& d, const TypeContext& types,
                              const FeatureArgs& features,
                              ValTypeVector* locals) {
  uint32_t numLocalEntries;
  if (!d.readVarU32(&numLocalEntries)) {
    return d.fail("failed to read number of local entries");
  }

  for (uint32_t i = 0; i < numLocalEntries; i++) {
    uint32_t count;
    if (!d.readVarU32(&count)) {
      return d.fail("failed to read local entry count");
    }

    if (MaxLocals - locals->length() < count) {
      return d.fail("too many locals");
    }

    ValType type;
    if (!d.readValType(types, features, &type)) {
      return false;
    }

    if (!type.isDefaultable()) {
      return d.fail("cannot have a non-defaultable local");
    }

    if (!locals->appendN(type, count)) {
      return false;
    }
  }

  return true;
}

bool wasm::DecodeValidatedLocalEntries(Decoder& d, ValTypeVector* locals) {
  uint32_t numLocalEntries;
  MOZ_ALWAYS_TRUE(d.readVarU32(&numLocalEntries));

  for (uint32_t i = 0; i < numLocalEntries; i++) {
    uint32_t count = d.uncheckedReadVarU32();
    MOZ_ASSERT(MaxLocals - locals->length() >= count);
    if (!locals->appendN(d.uncheckedReadValType(), count)) {
      return false;
    }
  }

  return true;
}

bool wasm::CheckIsSubtypeOf(Decoder& d, const ModuleEnvironment& env,
                            size_t opcodeOffset, ValType actual,
                            ValType expected, TypeCache* cache) {
  switch (env.types.isSubtypeOf(actual, expected, cache)) {
    case TypeResult::OOM:
      return false;
    case TypeResult::True:
      return true;
    case TypeResult::False: {
      UniqueChars actualText = ToString(actual);
      if (!actualText) {
        return false;
      }

      UniqueChars expectedText = ToString(expected);
      if (!expectedText) {
        return false;
      }

      UniqueChars error(
          JS_smprintf("type mismatch: expression has type %s but expected %s",
                      actualText.get(), expectedText.get()));
      if (!error) {
        return false;
      }

      return d.fail(opcodeOffset, error.get());
    }
    default:
      MOZ_CRASH();
  }
}

// Function body validation.

static bool DecodeFunctionBodyExprs(const ModuleEnvironment& env,
                                    uint32_t funcIndex,
                                    const ValTypeVector& locals,
                                    const uint8_t* bodyEnd, Decoder* d) {
  ValidatingOpIter iter(env, *d);

  if (!iter.startFunction(funcIndex)) {
    return false;
  }

#define CHECK(c)          \
  if (!(c)) return false; \
  break

  while (true) {
    OpBytes op;
    if (!iter.readOp(&op)) {
      return false;
    }

    Nothing nothing;
    NothingVector nothings{};
    ResultType unusedType;

    switch (op.b0) {
      case uint16_t(Op::End): {
        LabelKind unusedKind;
        if (!iter.readEnd(&unusedKind, &unusedType, &nothings, &nothings)) {
          return false;
        }
        iter.popEnd();
        if (iter.controlStackEmpty()) {
          return iter.endFunction(bodyEnd);
        }
        break;
      }
      case uint16_t(Op::Nop):
        CHECK(iter.readNop());
      case uint16_t(Op::Drop):
        CHECK(iter.readDrop());
      case uint16_t(Op::Call): {
        uint32_t unusedIndex;
        NothingVector unusedArgs{};
        CHECK(iter.readCall(&unusedIndex, &unusedArgs));
      }
      case uint16_t(Op::CallIndirect): {
        uint32_t unusedIndex, unusedIndex2;
        NothingVector unusedArgs{};
        CHECK(iter.readCallIndirect(&unusedIndex, &unusedIndex2, &nothing,
                                    &unusedArgs));
      }
      case uint16_t(Op::I32Const): {
        int32_t unused;
        CHECK(iter.readI32Const(&unused));
      }
      case uint16_t(Op::I64Const): {
        int64_t unused;
        CHECK(iter.readI64Const(&unused));
      }
      case uint16_t(Op::F32Const): {
        float unused;
        CHECK(iter.readF32Const(&unused));
      }
      case uint16_t(Op::F64Const): {
        double unused;
        CHECK(iter.readF64Const(&unused));
      }
      case uint16_t(Op::GetLocal): {
        uint32_t unused;
        CHECK(iter.readGetLocal(locals, &unused));
      }
      case uint16_t(Op::SetLocal): {
        uint32_t unused;
        CHECK(iter.readSetLocal(locals, &unused, &nothing));
      }
      case uint16_t(Op::TeeLocal): {
        uint32_t unused;
        CHECK(iter.readTeeLocal(locals, &unused, &nothing));
      }
      case uint16_t(Op::GetGlobal): {
        uint32_t unused;
        CHECK(iter.readGetGlobal(&unused));
      }
      case uint16_t(Op::SetGlobal): {
        uint32_t unused;
        CHECK(iter.readSetGlobal(&unused, &nothing));
      }
      case uint16_t(Op::TableGet): {
        uint32_t unusedTableIndex;
        CHECK(iter.readTableGet(&unusedTableIndex, &nothing));
      }
      case uint16_t(Op::TableSet): {
        uint32_t unusedTableIndex;
        CHECK(iter.readTableSet(&unusedTableIndex, &nothing, &nothing));
      }
      case uint16_t(Op::SelectNumeric): {
        StackType unused;
        CHECK(iter.readSelect(/*typed*/ false, &unused, &nothing, &nothing,
                              &nothing));
      }
      case uint16_t(Op::SelectTyped): {
        StackType unused;
        CHECK(iter.readSelect(/*typed*/ true, &unused, &nothing, &nothing,
                              &nothing));
      }
      case uint16_t(Op::Block):
        CHECK(iter.readBlock(&unusedType));
      case uint16_t(Op::Loop):
        CHECK(iter.readLoop(&unusedType));
      case uint16_t(Op::If):
        CHECK(iter.readIf(&unusedType, &nothing));
      case uint16_t(Op::Else):
        CHECK(iter.readElse(&unusedType, &unusedType, &nothings));
      case uint16_t(Op::I32Clz):
      case uint16_t(Op::I32Ctz):
      case uint16_t(Op::I32Popcnt):
        CHECK(iter.readUnary(ValType::I32, &nothing));
      case uint16_t(Op::I64Clz):
      case uint16_t(Op::I64Ctz):
      case uint16_t(Op::I64Popcnt):
        CHECK(iter.readUnary(ValType::I64, &nothing));
      case uint16_t(Op::F32Abs):
      case uint16_t(Op::F32Neg):
      case uint16_t(Op::F32Ceil):
      case uint16_t(Op::F32Floor):
      case uint16_t(Op::F32Sqrt):
      case uint16_t(Op::F32Trunc):
      case uint16_t(Op::F32Nearest):
        CHECK(iter.readUnary(ValType::F32, &nothing));
      case uint16_t(Op::F64Abs):
      case uint16_t(Op::F64Neg):
      case uint16_t(Op::F64Ceil):
      case uint16_t(Op::F64Floor):
      case uint16_t(Op::F64Sqrt):
      case uint16_t(Op::F64Trunc):
      case uint16_t(Op::F64Nearest):
        CHECK(iter.readUnary(ValType::F64, &nothing));
      case uint16_t(Op::I32Add):
      case uint16_t(Op::I32Sub):
      case uint16_t(Op::I32Mul):
      case uint16_t(Op::I32DivS):
      case uint16_t(Op::I32DivU):
      case uint16_t(Op::I32RemS):
      case uint16_t(Op::I32RemU):
      case uint16_t(Op::I32And):
      case uint16_t(Op::I32Or):
      case uint16_t(Op::I32Xor):
      case uint16_t(Op::I32Shl):
      case uint16_t(Op::I32ShrS):
      case uint16_t(Op::I32ShrU):
      case uint16_t(Op::I32Rotl):
      case uint16_t(Op::I32Rotr):
        CHECK(iter.readBinary(ValType::I32, &nothing, &nothing));
      case uint16_t(Op::I64Add):
      case uint16_t(Op::I64Sub):
      case uint16_t(Op::I64Mul):
      case uint16_t(Op::I64DivS):
      case uint16_t(Op::I64DivU):
      case uint16_t(Op::I64RemS):
      case uint16_t(Op::I64RemU):
      case uint16_t(Op::I64And):
      case uint16_t(Op::I64Or):
      case uint16_t(Op::I64Xor):
      case uint16_t(Op::I64Shl):
      case uint16_t(Op::I64ShrS):
      case uint16_t(Op::I64ShrU):
      case uint16_t(Op::I64Rotl):
      case uint16_t(Op::I64Rotr):
        CHECK(iter.readBinary(ValType::I64, &nothing, &nothing));
      case uint16_t(Op::F32Add):
      case uint16_t(Op::F32Sub):
      case uint16_t(Op::F32Mul):
      case uint16_t(Op::F32Div):
      case uint16_t(Op::F32Min):
      case uint16_t(Op::F32Max):
      case uint16_t(Op::F32CopySign):
        CHECK(iter.readBinary(ValType::F32, &nothing, &nothing));
      case uint16_t(Op::F64Add):
      case uint16_t(Op::F64Sub):
      case uint16_t(Op::F64Mul):
      case uint16_t(Op::F64Div):
      case uint16_t(Op::F64Min):
      case uint16_t(Op::F64Max):
      case uint16_t(Op::F64CopySign):
        CHECK(iter.readBinary(ValType::F64, &nothing, &nothing));
      case uint16_t(Op::I32Eq):
      case uint16_t(Op::I32Ne):
      case uint16_t(Op::I32LtS):
      case uint16_t(Op::I32LtU):
      case uint16_t(Op::I32LeS):
      case uint16_t(Op::I32LeU):
      case uint16_t(Op::I32GtS):
      case uint16_t(Op::I32GtU):
      case uint16_t(Op::I32GeS):
      case uint16_t(Op::I32GeU):
        CHECK(iter.readComparison(ValType::I32, &nothing, &nothing));
      case uint16_t(Op::I64Eq):
      case uint16_t(Op::I64Ne):
      case uint16_t(Op::I64LtS):
      case uint16_t(Op::I64LtU):
      case uint16_t(Op::I64LeS):
      case uint16_t(Op::I64LeU):
      case uint16_t(Op::I64GtS):
      case uint16_t(Op::I64GtU):
      case uint16_t(Op::I64GeS):
      case uint16_t(Op::I64GeU):
        CHECK(iter.readComparison(ValType::I64, &nothing, &nothing));
      case uint16_t(Op::F32Eq):
      case uint16_t(Op::F32Ne):
      case uint16_t(Op::F32Lt):
      case uint16_t(Op::F32Le):
      case uint16_t(Op::F32Gt):
      case uint16_t(Op::F32Ge):
        CHECK(iter.readComparison(ValType::F32, &nothing, &nothing));
      case uint16_t(Op::F64Eq):
      case uint16_t(Op::F64Ne):
      case uint16_t(Op::F64Lt):
      case uint16_t(Op::F64Le):
      case uint16_t(Op::F64Gt):
      case uint16_t(Op::F64Ge):
        CHECK(iter.readComparison(ValType::F64, &nothing, &nothing));
      case uint16_t(Op::I32Eqz):
        CHECK(iter.readConversion(ValType::I32, ValType::I32, &nothing));
      case uint16_t(Op::I64Eqz):
      case uint16_t(Op::I32WrapI64):
        CHECK(iter.readConversion(ValType::I64, ValType::I32, &nothing));
      case uint16_t(Op::I32TruncSF32):
      case uint16_t(Op::I32TruncUF32):
      case uint16_t(Op::I32ReinterpretF32):
        CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
      case uint16_t(Op::I32TruncSF64):
      case uint16_t(Op::I32TruncUF64):
        CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
      case uint16_t(Op::I64ExtendSI32):
      case uint16_t(Op::I64ExtendUI32):
        CHECK(iter.readConversion(ValType::I32, ValType::I64, &nothing));
      case uint16_t(Op::I64TruncSF32):
      case uint16_t(Op::I64TruncUF32):
        CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
      case uint16_t(Op::I64TruncSF64):
      case uint16_t(Op::I64TruncUF64):
      case uint16_t(Op::I64ReinterpretF64):
        CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
      case uint16_t(Op::F32ConvertSI32):
      case uint16_t(Op::F32ConvertUI32):
      case uint16_t(Op::F32ReinterpretI32):
        CHECK(iter.readConversion(ValType::I32, ValType::F32, &nothing));
      case uint16_t(Op::F32ConvertSI64):
      case uint16_t(Op::F32ConvertUI64):
        CHECK(iter.readConversion(ValType::I64, ValType::F32, &nothing));
      case uint16_t(Op::F32DemoteF64):
        CHECK(iter.readConversion(ValType::F64, ValType::F32, &nothing));
      case uint16_t(Op::F64ConvertSI32):
      case uint16_t(Op::F64ConvertUI32):
        CHECK(iter.readConversion(ValType::I32, ValType::F64, &nothing));
      case uint16_t(Op::F64ConvertSI64):
      case uint16_t(Op::F64ConvertUI64):
      case uint16_t(Op::F64ReinterpretI64):
        CHECK(iter.readConversion(ValType::I64, ValType::F64, &nothing));
      case uint16_t(Op::F64PromoteF32):
        CHECK(iter.readConversion(ValType::F32, ValType::F64, &nothing));
      case uint16_t(Op::I32Extend8S):
      case uint16_t(Op::I32Extend16S):
        CHECK(iter.readConversion(ValType::I32, ValType::I32, &nothing));
      case uint16_t(Op::I64Extend8S):
      case uint16_t(Op::I64Extend16S):
      case uint16_t(Op::I64Extend32S):
        CHECK(iter.readConversion(ValType::I64, ValType::I64, &nothing));
      case uint16_t(Op::I32Load8S):
      case uint16_t(Op::I32Load8U): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I32, 1, &addr));
      }
      case uint16_t(Op::I32Load16S):
      case uint16_t(Op::I32Load16U): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I32, 2, &addr));
      }
      case uint16_t(Op::I32Load): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I32, 4, &addr));
      }
      case uint16_t(Op::I64Load8S):
      case uint16_t(Op::I64Load8U): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I64, 1, &addr));
      }
      case uint16_t(Op::I64Load16S):
      case uint16_t(Op::I64Load16U): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I64, 2, &addr));
      }
      case uint16_t(Op::I64Load32S):
      case uint16_t(Op::I64Load32U): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I64, 4, &addr));
      }
      case uint16_t(Op::I64Load): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::I64, 8, &addr));
      }
      case uint16_t(Op::F32Load): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::F32, 4, &addr));
      }
      case uint16_t(Op::F64Load): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readLoad(ValType::F64, 8, &addr));
      }
      case uint16_t(Op::I32Store8): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I32, 1, &addr, &nothing));
      }
      case uint16_t(Op::I32Store16): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I32, 2, &addr, &nothing));
      }
      case uint16_t(Op::I32Store): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I32, 4, &addr, &nothing));
      }
      case uint16_t(Op::I64Store8): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I64, 1, &addr, &nothing));
      }
      case uint16_t(Op::I64Store16): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I64, 2, &addr, &nothing));
      }
      case uint16_t(Op::I64Store32): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I64, 4, &addr, &nothing));
      }
      case uint16_t(Op::I64Store): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::I64, 8, &addr, &nothing));
      }
      case uint16_t(Op::F32Store): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::F32, 4, &addr, &nothing));
      }
      case uint16_t(Op::F64Store): {
        LinearMemoryAddress<Nothing> addr;
        CHECK(iter.readStore(ValType::F64, 8, &addr, &nothing));
      }
      case uint16_t(Op::MemoryGrow):
        CHECK(iter.readMemoryGrow(&nothing));
      case uint16_t(Op::MemorySize):
        CHECK(iter.readMemorySize());
      case uint16_t(Op::Br): {
        uint32_t unusedDepth;
        CHECK(iter.readBr(&unusedDepth, &unusedType, &nothings));
      }
      case uint16_t(Op::BrIf): {
        uint32_t unusedDepth;
        CHECK(iter.readBrIf(&unusedDepth, &unusedType, &nothings, &nothing));
      }
      case uint16_t(Op::BrTable): {
        Uint32Vector unusedDepths;
        uint32_t unusedDefault;
        CHECK(iter.readBrTable(&unusedDepths, &unusedDefault, &unusedType,
                               &nothings, &nothing));
      }
      case uint16_t(Op::Return):
        CHECK(iter.readReturn(&nothings));
      case uint16_t(Op::Unreachable):
        CHECK(iter.readUnreachable());
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::GcPrefix): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(GcOp::StructNewWithRtt): {
            uint32_t unusedUint;
            NothingVector unusedArgs{};
            CHECK(
                iter.readStructNewWithRtt(&unusedUint, &nothing, &unusedArgs));
          }
          case uint32_t(GcOp::StructNewDefaultWithRtt): {
            uint32_t unusedUint;
            CHECK(iter.readStructNewDefaultWithRtt(&unusedUint, &nothing));
          }
          case uint32_t(GcOp::StructGet): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldExtension::None, &nothing));
          }
          case uint32_t(GcOp::StructGetS): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldExtension::Signed, &nothing));
          }
          case uint32_t(GcOp::StructGetU): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldExtension::Unsigned, &nothing));
          }
          case uint32_t(GcOp::StructSet): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructSet(&unusedUint1, &unusedUint2, &nothing,
                                     &nothing));
          }
          case uint32_t(GcOp::ArrayNewWithRtt): {
            uint32_t unusedUint;
            CHECK(iter.readArrayNewWithRtt(&unusedUint, &nothing, &nothing,
                                           &nothing));
          }
          case uint32_t(GcOp::ArrayNewDefaultWithRtt): {
            uint32_t unusedUint;
            CHECK(iter.readArrayNewDefaultWithRtt(&unusedUint, &nothing,
                                                  &nothing));
          }
          case uint32_t(GcOp::ArrayGet): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldExtension::None,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayGetS): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldExtension::Signed,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayGetU): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldExtension::Unsigned,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArraySet): {
            uint32_t unusedUint1;
            CHECK(
                iter.readArraySet(&unusedUint1, &nothing, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayLen): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayLen(&unusedUint1, &nothing));
          }
          case uint16_t(GcOp::RttCanon): {
            ValType unusedTy;
            CHECK(iter.readRttCanon(&unusedTy));
          }
          case uint16_t(GcOp::RttSub): {
            CHECK(iter.readRttSub(&nothing));
          }
          case uint16_t(GcOp::RefTest): {
            uint32_t unusedRttTypeIndex;
            uint32_t unusedRttDepth;
            CHECK(iter.readRefTest(&nothing, &unusedRttTypeIndex,
                                   &unusedRttDepth, &nothing));
          }
          case uint16_t(GcOp::RefCast): {
            uint32_t unusedRttTypeIndex;
            uint32_t unusedRttDepth;
            CHECK(iter.readRefCast(&nothing, &unusedRttTypeIndex,
                                   &unusedRttDepth, &nothing));
          }
          case uint16_t(GcOp::BrOnCast): {
            uint32_t unusedRelativeDepth;
            uint32_t unusedRttTypeIndex;
            uint32_t unusedRttDepth;
            CHECK(iter.readBrOnCast(&unusedRelativeDepth, &nothing,
                                    &unusedRttTypeIndex, &unusedRttDepth,
                                    &unusedType, &nothings));
          }
          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
#endif

#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        if (!env.v128Enabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t noIndex;
        switch (op.b1) {
          case uint32_t(SimdOp::I8x16ExtractLaneS):
          case uint32_t(SimdOp::I8x16ExtractLaneU):
            CHECK(iter.readExtractLane(ValType::I32, 16, &noIndex, &nothing));
          case uint32_t(SimdOp::I16x8ExtractLaneS):
          case uint32_t(SimdOp::I16x8ExtractLaneU):
            CHECK(iter.readExtractLane(ValType::I32, 8, &noIndex, &nothing));
          case uint32_t(SimdOp::I32x4ExtractLane):
            CHECK(iter.readExtractLane(ValType::I32, 4, &noIndex, &nothing));
          case uint32_t(SimdOp::I64x2ExtractLane):
            CHECK(iter.readExtractLane(ValType::I64, 2, &noIndex, &nothing));
          case uint32_t(SimdOp::F32x4ExtractLane):
            CHECK(iter.readExtractLane(ValType::F32, 4, &noIndex, &nothing));
          case uint32_t(SimdOp::F64x2ExtractLane):
            CHECK(iter.readExtractLane(ValType::F64, 2, &noIndex, &nothing));

          case uint32_t(SimdOp::I8x16Splat):
          case uint32_t(SimdOp::I16x8Splat):
          case uint32_t(SimdOp::I32x4Splat):
            CHECK(iter.readConversion(ValType::I32, ValType::V128, &nothing));
          case uint32_t(SimdOp::I64x2Splat):
            CHECK(iter.readConversion(ValType::I64, ValType::V128, &nothing));
          case uint32_t(SimdOp::F32x4Splat):
            CHECK(iter.readConversion(ValType::F32, ValType::V128, &nothing));
          case uint32_t(SimdOp::F64x2Splat):
            CHECK(iter.readConversion(ValType::F64, ValType::V128, &nothing));

          case uint32_t(SimdOp::V128AnyTrue):
          case uint32_t(SimdOp::I8x16AllTrue):
          case uint32_t(SimdOp::I16x8AllTrue):
          case uint32_t(SimdOp::I32x4AllTrue):
          case uint32_t(SimdOp::I64x2AllTrue):
          case uint32_t(SimdOp::I8x16Bitmask):
          case uint32_t(SimdOp::I16x8Bitmask):
          case uint32_t(SimdOp::I32x4Bitmask):
          case uint32_t(SimdOp::I64x2Bitmask):
            CHECK(iter.readConversion(ValType::V128, ValType::I32, &nothing));

          case uint32_t(SimdOp::I8x16ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::I32, 16, &noIndex, &nothing,
                                       &nothing));
          case uint32_t(SimdOp::I16x8ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::I32, 8, &noIndex, &nothing,
                                       &nothing));
          case uint32_t(SimdOp::I32x4ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::I32, 4, &noIndex, &nothing,
                                       &nothing));
          case uint32_t(SimdOp::I64x2ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::I64, 2, &noIndex, &nothing,
                                       &nothing));
          case uint32_t(SimdOp::F32x4ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::F32, 4, &noIndex, &nothing,
                                       &nothing));
          case uint32_t(SimdOp::F64x2ReplaceLane):
            CHECK(iter.readReplaceLane(ValType::F64, 2, &noIndex, &nothing,
                                       &nothing));

          case uint32_t(SimdOp::I8x16Eq):
          case uint32_t(SimdOp::I8x16Ne):
          case uint32_t(SimdOp::I8x16LtS):
          case uint32_t(SimdOp::I8x16LtU):
          case uint32_t(SimdOp::I8x16GtS):
          case uint32_t(SimdOp::I8x16GtU):
          case uint32_t(SimdOp::I8x16LeS):
          case uint32_t(SimdOp::I8x16LeU):
          case uint32_t(SimdOp::I8x16GeS):
          case uint32_t(SimdOp::I8x16GeU):
          case uint32_t(SimdOp::I16x8Eq):
          case uint32_t(SimdOp::I16x8Ne):
          case uint32_t(SimdOp::I16x8LtS):
          case uint32_t(SimdOp::I16x8LtU):
          case uint32_t(SimdOp::I16x8GtS):
          case uint32_t(SimdOp::I16x8GtU):
          case uint32_t(SimdOp::I16x8LeS):
          case uint32_t(SimdOp::I16x8LeU):
          case uint32_t(SimdOp::I16x8GeS):
          case uint32_t(SimdOp::I16x8GeU):
          case uint32_t(SimdOp::I32x4Eq):
          case uint32_t(SimdOp::I32x4Ne):
          case uint32_t(SimdOp::I32x4LtS):
          case uint32_t(SimdOp::I32x4LtU):
          case uint32_t(SimdOp::I32x4GtS):
          case uint32_t(SimdOp::I32x4GtU):
          case uint32_t(SimdOp::I32x4LeS):
          case uint32_t(SimdOp::I32x4LeU):
          case uint32_t(SimdOp::I32x4GeS):
          case uint32_t(SimdOp::I32x4GeU):
          case uint32_t(SimdOp::I64x2Eq):
          case uint32_t(SimdOp::I64x2Ne):
          case uint32_t(SimdOp::I64x2LtS):
          case uint32_t(SimdOp::I64x2GtS):
          case uint32_t(SimdOp::I64x2LeS):
          case uint32_t(SimdOp::I64x2GeS):
          case uint32_t(SimdOp::F32x4Eq):
          case uint32_t(SimdOp::F32x4Ne):
          case uint32_t(SimdOp::F32x4Lt):
          case uint32_t(SimdOp::F32x4Gt):
          case uint32_t(SimdOp::F32x4Le):
          case uint32_t(SimdOp::F32x4Ge):
          case uint32_t(SimdOp::F64x2Eq):
          case uint32_t(SimdOp::F64x2Ne):
          case uint32_t(SimdOp::F64x2Lt):
          case uint32_t(SimdOp::F64x2Gt):
          case uint32_t(SimdOp::F64x2Le):
          case uint32_t(SimdOp::F64x2Ge):
          case uint32_t(SimdOp::V128And):
          case uint32_t(SimdOp::V128Or):
          case uint32_t(SimdOp::V128Xor):
          case uint32_t(SimdOp::V128AndNot):
          case uint32_t(SimdOp::I8x16AvgrU):
          case uint32_t(SimdOp::I16x8AvgrU):
          case uint32_t(SimdOp::I8x16Add):
          case uint32_t(SimdOp::I8x16AddSaturateS):
          case uint32_t(SimdOp::I8x16AddSaturateU):
          case uint32_t(SimdOp::I8x16Sub):
          case uint32_t(SimdOp::I8x16SubSaturateS):
          case uint32_t(SimdOp::I8x16SubSaturateU):
          case uint32_t(SimdOp::I8x16MinS):
          case uint32_t(SimdOp::I8x16MinU):
          case uint32_t(SimdOp::I8x16MaxS):
          case uint32_t(SimdOp::I8x16MaxU):
          case uint32_t(SimdOp::I16x8Add):
          case uint32_t(SimdOp::I16x8AddSaturateS):
          case uint32_t(SimdOp::I16x8AddSaturateU):
          case uint32_t(SimdOp::I16x8Sub):
          case uint32_t(SimdOp::I16x8SubSaturateS):
          case uint32_t(SimdOp::I16x8SubSaturateU):
          case uint32_t(SimdOp::I16x8Mul):
          case uint32_t(SimdOp::I16x8MinS):
          case uint32_t(SimdOp::I16x8MinU):
          case uint32_t(SimdOp::I16x8MaxS):
          case uint32_t(SimdOp::I16x8MaxU):
          case uint32_t(SimdOp::I32x4Add):
          case uint32_t(SimdOp::I32x4Sub):
          case uint32_t(SimdOp::I32x4Mul):
          case uint32_t(SimdOp::I32x4MinS):
          case uint32_t(SimdOp::I32x4MinU):
          case uint32_t(SimdOp::I32x4MaxS):
          case uint32_t(SimdOp::I32x4MaxU):
          case uint32_t(SimdOp::I64x2Add):
          case uint32_t(SimdOp::I64x2Sub):
          case uint32_t(SimdOp::I64x2Mul):
          case uint32_t(SimdOp::F32x4Add):
          case uint32_t(SimdOp::F32x4Sub):
          case uint32_t(SimdOp::F32x4Mul):
          case uint32_t(SimdOp::F32x4Div):
          case uint32_t(SimdOp::F32x4Min):
          case uint32_t(SimdOp::F32x4Max):
          case uint32_t(SimdOp::F64x2Add):
          case uint32_t(SimdOp::F64x2Sub):
          case uint32_t(SimdOp::F64x2Mul):
          case uint32_t(SimdOp::F64x2Div):
          case uint32_t(SimdOp::F64x2Min):
          case uint32_t(SimdOp::F64x2Max):
          case uint32_t(SimdOp::I8x16NarrowSI16x8):
          case uint32_t(SimdOp::I8x16NarrowUI16x8):
          case uint32_t(SimdOp::I16x8NarrowSI32x4):
          case uint32_t(SimdOp::I16x8NarrowUI32x4):
          case uint32_t(SimdOp::V8x16Swizzle):
          case uint32_t(SimdOp::F32x4PMax):
          case uint32_t(SimdOp::F32x4PMin):
          case uint32_t(SimdOp::F64x2PMax):
          case uint32_t(SimdOp::F64x2PMin):
          case uint32_t(SimdOp::I32x4DotSI16x8):
          case uint32_t(SimdOp::I16x8ExtMulLowSI8x16):
          case uint32_t(SimdOp::I16x8ExtMulHighSI8x16):
          case uint32_t(SimdOp::I16x8ExtMulLowUI8x16):
          case uint32_t(SimdOp::I16x8ExtMulHighUI8x16):
          case uint32_t(SimdOp::I32x4ExtMulLowSI16x8):
          case uint32_t(SimdOp::I32x4ExtMulHighSI16x8):
          case uint32_t(SimdOp::I32x4ExtMulLowUI16x8):
          case uint32_t(SimdOp::I32x4ExtMulHighUI16x8):
          case uint32_t(SimdOp::I64x2ExtMulLowSI32x4):
          case uint32_t(SimdOp::I64x2ExtMulHighSI32x4):
          case uint32_t(SimdOp::I64x2ExtMulLowUI32x4):
          case uint32_t(SimdOp::I64x2ExtMulHighUI32x4):
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK(iter.readBinary(ValType::V128, &nothing, &nothing));

          case uint32_t(SimdOp::I8x16Neg):
          case uint32_t(SimdOp::I16x8Neg):
          case uint32_t(SimdOp::I16x8WidenLowSI8x16):
          case uint32_t(SimdOp::I16x8WidenHighSI8x16):
          case uint32_t(SimdOp::I16x8WidenLowUI8x16):
          case uint32_t(SimdOp::I16x8WidenHighUI8x16):
          case uint32_t(SimdOp::I32x4Neg):
          case uint32_t(SimdOp::I32x4WidenLowSI16x8):
          case uint32_t(SimdOp::I32x4WidenHighSI16x8):
          case uint32_t(SimdOp::I32x4WidenLowUI16x8):
          case uint32_t(SimdOp::I32x4WidenHighUI16x8):
          case uint32_t(SimdOp::I32x4TruncSSatF32x4):
          case uint32_t(SimdOp::I32x4TruncUSatF32x4):
          case uint32_t(SimdOp::I64x2Neg):
          case uint32_t(SimdOp::I64x2WidenLowSI32x4):
          case uint32_t(SimdOp::I64x2WidenHighSI32x4):
          case uint32_t(SimdOp::I64x2WidenLowUI32x4):
          case uint32_t(SimdOp::I64x2WidenHighUI32x4):
          case uint32_t(SimdOp::F32x4Abs):
          case uint32_t(SimdOp::F32x4Neg):
          case uint32_t(SimdOp::F32x4Sqrt):
          case uint32_t(SimdOp::F32x4ConvertSI32x4):
          case uint32_t(SimdOp::F32x4ConvertUI32x4):
          case uint32_t(SimdOp::F64x2Abs):
          case uint32_t(SimdOp::F64x2Neg):
          case uint32_t(SimdOp::F64x2Sqrt):
          case uint32_t(SimdOp::V128Not):
          case uint32_t(SimdOp::I8x16Popcnt):
          case uint32_t(SimdOp::I8x16Abs):
          case uint32_t(SimdOp::I16x8Abs):
          case uint32_t(SimdOp::I32x4Abs):
          case uint32_t(SimdOp::I64x2Abs):
          case uint32_t(SimdOp::F32x4Ceil):
          case uint32_t(SimdOp::F32x4Floor):
          case uint32_t(SimdOp::F32x4Trunc):
          case uint32_t(SimdOp::F32x4Nearest):
          case uint32_t(SimdOp::F64x2Ceil):
          case uint32_t(SimdOp::F64x2Floor):
          case uint32_t(SimdOp::F64x2Trunc):
          case uint32_t(SimdOp::F64x2Nearest):
          case uint32_t(SimdOp::F32x4DemoteF64x2Zero):
          case uint32_t(SimdOp::F64x2PromoteLowF32x4):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4S):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4U):
          case uint32_t(SimdOp::I32x4TruncSatF64x2SZero):
          case uint32_t(SimdOp::I32x4TruncSatF64x2UZero):
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16S):
          case uint32_t(SimdOp::I16x8ExtAddPairwiseI8x16U):
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8S):
          case uint32_t(SimdOp::I32x4ExtAddPairwiseI16x8U):
            CHECK(iter.readUnary(ValType::V128, &nothing));

          case uint32_t(SimdOp::I8x16Shl):
          case uint32_t(SimdOp::I8x16ShrS):
          case uint32_t(SimdOp::I8x16ShrU):
          case uint32_t(SimdOp::I16x8Shl):
          case uint32_t(SimdOp::I16x8ShrS):
          case uint32_t(SimdOp::I16x8ShrU):
          case uint32_t(SimdOp::I32x4Shl):
          case uint32_t(SimdOp::I32x4ShrS):
          case uint32_t(SimdOp::I32x4ShrU):
          case uint32_t(SimdOp::I64x2Shl):
          case uint32_t(SimdOp::I64x2ShrS):
          case uint32_t(SimdOp::I64x2ShrU):
            CHECK(iter.readVectorShift(&nothing, &nothing));

          case uint32_t(SimdOp::V128Bitselect):
            CHECK(iter.readVectorSelect(&nothing, &nothing, &nothing));

          case uint32_t(SimdOp::V8x16Shuffle): {
            V128 mask;
            CHECK(iter.readVectorShuffle(&nothing, &nothing, &mask));
          }

          case uint32_t(SimdOp::V128Const): {
            V128 noVector;
            CHECK(iter.readV128Const(&noVector));
          }

          case uint32_t(SimdOp::V128Load): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoad(ValType::V128, 16, &addr));
          }

          case uint32_t(SimdOp::V8x16LoadSplat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(1, &addr));
          }

          case uint32_t(SimdOp::V16x8LoadSplat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(2, &addr));
          }

          case uint32_t(SimdOp::V32x4LoadSplat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(4, &addr));
          }

          case uint32_t(SimdOp::V64x2LoadSplat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(8, &addr));
          }

          case uint32_t(SimdOp::I16x8LoadS8x8):
          case uint32_t(SimdOp::I16x8LoadU8x8): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadExtend(&addr));
          }

          case uint32_t(SimdOp::I32x4LoadS16x4):
          case uint32_t(SimdOp::I32x4LoadU16x4): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadExtend(&addr));
          }

          case uint32_t(SimdOp::I64x2LoadS32x2):
          case uint32_t(SimdOp::I64x2LoadU32x2): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadExtend(&addr));
          }

          case uint32_t(SimdOp::V128Store): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStore(ValType::V128, 16, &addr, &nothing));
          }

          case uint32_t(SimdOp::V128Load32Zero): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(4, &addr));
          }

          case uint32_t(SimdOp::V128Load64Zero): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(8, &addr));
          }

          case uint32_t(SimdOp::V128Load8Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadLane(1, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Load16Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadLane(2, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Load32Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadLane(4, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Load64Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadLane(8, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Store8Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStoreLane(1, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Store16Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStoreLane(2, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Store32Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStoreLane(4, &addr, &noIndex, &nothing));
          }

          case uint32_t(SimdOp::V128Store64Lane): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readStoreLane(8, &addr, &noIndex, &nothing));
          }

          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
#endif  // ENABLE_WASM_SIMD

      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSSatF32):
          case uint32_t(MiscOp::I32TruncUSatF32):
            CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
          case uint32_t(MiscOp::I32TruncSSatF64):
          case uint32_t(MiscOp::I32TruncUSatF64):
            CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
          case uint32_t(MiscOp::I64TruncSSatF32):
          case uint32_t(MiscOp::I64TruncUSatF32):
            CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
          case uint32_t(MiscOp::I64TruncSSatF64):
          case uint32_t(MiscOp::I64TruncUSatF64):
            CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
          case uint32_t(MiscOp::MemCopy): {
            uint32_t unusedDestMemIndex;
            uint32_t unusedSrcMemIndex;
            CHECK(iter.readMemOrTableCopy(/*isMem=*/true, &unusedDestMemIndex,
                                          &nothing, &unusedSrcMemIndex,
                                          &nothing, &nothing));
          }
          case uint32_t(MiscOp::DataDrop): {
            uint32_t unusedSegIndex;
            CHECK(iter.readDataOrElemDrop(/*isData=*/true, &unusedSegIndex));
          }
          case uint32_t(MiscOp::MemFill):
            CHECK(iter.readMemFill(&nothing, &nothing, &nothing));
          case uint32_t(MiscOp::MemInit): {
            uint32_t unusedSegIndex;
            uint32_t unusedTableIndex;
            CHECK(iter.readMemOrTableInit(/*isMem=*/true, &unusedSegIndex,
                                          &unusedTableIndex, &nothing, &nothing,
                                          &nothing));
          }
          case uint32_t(MiscOp::TableCopy): {
            uint32_t unusedDestTableIndex;
            uint32_t unusedSrcTableIndex;
            CHECK(iter.readMemOrTableCopy(
                /*isMem=*/false, &unusedDestTableIndex, &nothing,
                &unusedSrcTableIndex, &nothing, &nothing));
          }
          case uint32_t(MiscOp::ElemDrop): {
            uint32_t unusedSegIndex;
            CHECK(iter.readDataOrElemDrop(/*isData=*/false, &unusedSegIndex));
          }
          case uint32_t(MiscOp::TableInit): {
            uint32_t unusedSegIndex;
            uint32_t unusedTableIndex;
            CHECK(iter.readMemOrTableInit(/*isMem=*/false, &unusedSegIndex,
                                          &unusedTableIndex, &nothing, &nothing,
                                          &nothing));
          }
          case uint32_t(MiscOp::TableFill): {
            uint32_t unusedTableIndex;
            CHECK(iter.readTableFill(&unusedTableIndex, &nothing, &nothing,
                                     &nothing));
          }
          case uint32_t(MiscOp::TableGrow): {
            uint32_t unusedTableIndex;
            CHECK(iter.readTableGrow(&unusedTableIndex, &nothing, &nothing));
          }
          case uint32_t(MiscOp::TableSize): {
            uint32_t unusedTableIndex;
            CHECK(iter.readTableSize(&unusedTableIndex));
          }
          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
      case uint16_t(Op::RefAsNonNull): {
        if (!env.functionReferencesEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        CHECK(iter.readRefAsNonNull(&nothing));
      }
      case uint16_t(Op::BrOnNull): {
        if (!env.functionReferencesEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedDepth;
        CHECK(
            iter.readBrOnNull(&unusedDepth, &unusedType, &nothings, &nothing));
      }
#endif
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefEq): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        CHECK(iter.readComparison(RefType::eq(), &nothing, &nothing));
      }
#endif
      case uint16_t(Op::RefFunc): {
        uint32_t unusedIndex;
        CHECK(iter.readRefFunc(&unusedIndex));
      }
      case uint16_t(Op::RefNull): {
        RefType type;
        CHECK(iter.readRefNull(&type));
      }
      case uint16_t(Op::RefIsNull): {
        Nothing nothing;
        CHECK(iter.readRefIsNull(&nothing));
      }
#ifdef ENABLE_WASM_EXCEPTIONS
      case uint16_t(Op::Try):
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        CHECK(iter.readTry(&unusedType));
      case uint16_t(Op::Catch): {
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        LabelKind unusedKind;
        uint32_t unusedIndex;
        CHECK(iter.readCatch(&unusedKind, &unusedIndex, &unusedType,
                             &unusedType, &nothings));
      }
      case uint16_t(Op::CatchAll): {
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        LabelKind unusedKind;
        CHECK(iter.readCatchAll(&unusedKind, &unusedType, &unusedType,
                                &nothings));
      }
      case uint16_t(Op::Delegate): {
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedDepth;
        if (!iter.readDelegate(&unusedDepth, &unusedType, &nothings)) {
          return false;
        }
        iter.popDelegate();
        break;
      }
      case uint16_t(Op::Throw): {
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedIndex;
        CHECK(iter.readThrow(&unusedIndex, &nothings));
      }
      case uint16_t(Op::Rethrow): {
        if (!env.exceptionsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedDepth;
        CHECK(iter.readRethrow(&unusedDepth));
      }
#endif
      case uint16_t(Op::ThreadPrefix): {
        // Though thread ops can be used on nonshared memories, we make them
        // unavailable if shared memory has been disabled in the prefs, for
        // maximum predictability and safety and consistency with JS.
        if (env.sharedMemoryEnabled() == Shareable::False) {
          return iter.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(ThreadOp::Wake): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readWake(&addr, &nothing));
          }
          case uint32_t(ThreadOp::I32Wait): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readWait(&addr, ValType::I32, 4, &nothing, &nothing));
          }
          case uint32_t(ThreadOp::I64Wait): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readWait(&addr, ValType::I64, 8, &nothing, &nothing));
          }
          case uint32_t(ThreadOp::Fence): {
            CHECK(iter.readFence());
          }
          case uint32_t(ThreadOp::I32AtomicLoad): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I32, 4));
          }
          case uint32_t(ThreadOp::I64AtomicLoad): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I64, 8));
          }
          case uint32_t(ThreadOp::I32AtomicLoad8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I32, 1));
          }
          case uint32_t(ThreadOp::I32AtomicLoad16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I32, 2));
          }
          case uint32_t(ThreadOp::I64AtomicLoad8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I64, 1));
          }
          case uint32_t(ThreadOp::I64AtomicLoad16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I64, 2));
          }
          case uint32_t(ThreadOp::I64AtomicLoad32U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicLoad(&addr, ValType::I64, 4));
          }
          case uint32_t(ThreadOp::I32AtomicStore): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I32, 4, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicStore): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I64, 8, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicStore8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I32, 1, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicStore16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I32, 2, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicStore8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I64, 1, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicStore16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I64, 2, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicStore32U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicStore(&addr, ValType::I64, 4, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicAdd):
          case uint32_t(ThreadOp::I32AtomicSub):
          case uint32_t(ThreadOp::I32AtomicAnd):
          case uint32_t(ThreadOp::I32AtomicOr):
          case uint32_t(ThreadOp::I32AtomicXor):
          case uint32_t(ThreadOp::I32AtomicXchg): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I32, 4, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicAdd):
          case uint32_t(ThreadOp::I64AtomicSub):
          case uint32_t(ThreadOp::I64AtomicAnd):
          case uint32_t(ThreadOp::I64AtomicOr):
          case uint32_t(ThreadOp::I64AtomicXor):
          case uint32_t(ThreadOp::I64AtomicXchg): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I64, 8, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicAdd8U):
          case uint32_t(ThreadOp::I32AtomicSub8U):
          case uint32_t(ThreadOp::I32AtomicAnd8U):
          case uint32_t(ThreadOp::I32AtomicOr8U):
          case uint32_t(ThreadOp::I32AtomicXor8U):
          case uint32_t(ThreadOp::I32AtomicXchg8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I32, 1, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicAdd16U):
          case uint32_t(ThreadOp::I32AtomicSub16U):
          case uint32_t(ThreadOp::I32AtomicAnd16U):
          case uint32_t(ThreadOp::I32AtomicOr16U):
          case uint32_t(ThreadOp::I32AtomicXor16U):
          case uint32_t(ThreadOp::I32AtomicXchg16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I32, 2, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicAdd8U):
          case uint32_t(ThreadOp::I64AtomicSub8U):
          case uint32_t(ThreadOp::I64AtomicAnd8U):
          case uint32_t(ThreadOp::I64AtomicOr8U):
          case uint32_t(ThreadOp::I64AtomicXor8U):
          case uint32_t(ThreadOp::I64AtomicXchg8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I64, 1, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicAdd16U):
          case uint32_t(ThreadOp::I64AtomicSub16U):
          case uint32_t(ThreadOp::I64AtomicAnd16U):
          case uint32_t(ThreadOp::I64AtomicOr16U):
          case uint32_t(ThreadOp::I64AtomicXor16U):
          case uint32_t(ThreadOp::I64AtomicXchg16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I64, 2, &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicAdd32U):
          case uint32_t(ThreadOp::I64AtomicSub32U):
          case uint32_t(ThreadOp::I64AtomicAnd32U):
          case uint32_t(ThreadOp::I64AtomicOr32U):
          case uint32_t(ThreadOp::I64AtomicXor32U):
          case uint32_t(ThreadOp::I64AtomicXchg32U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicRMW(&addr, ValType::I64, 4, &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicCmpXchg): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 4, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicCmpXchg): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 8, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicCmpXchg8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 1, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I32AtomicCmpXchg16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I32, 2, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicCmpXchg8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 1, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicCmpXchg16U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 2, &nothing,
                                         &nothing));
          }
          case uint32_t(ThreadOp::I64AtomicCmpXchg32U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readAtomicCmpXchg(&addr, ValType::I64, 4, &nothing,
                                         &nothing));
          }
          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
      case uint16_t(Op::MozPrefix):
        return iter.unrecognizedOpcode(&op);
      default:
        return iter.unrecognizedOpcode(&op);
    }
  }

  MOZ_CRASH("unreachable");

#undef CHECK
}

bool wasm::ValidateFunctionBody(const ModuleEnvironment& env,
                                uint32_t funcIndex, uint32_t bodySize,
                                Decoder& d) {
  ValTypeVector locals;
  if (!locals.appendAll(env.funcs[funcIndex].type->args())) {
    return false;
  }

  const uint8_t* bodyBegin = d.currentPosition();

  if (!DecodeLocalEntries(d, env.types, env.features, &locals)) {
    return false;
  }

  if (!DecodeFunctionBodyExprs(env, funcIndex, locals, bodyBegin + bodySize,
                               &d)) {
    return false;
  }

  return true;
}

// Section macros.

static bool DecodePreamble(Decoder& d) {
  if (d.bytesRemain() > MaxModuleBytes) {
    return d.fail("module too big");
  }

  uint32_t u32;
  if (!d.readFixedU32(&u32) || u32 != MagicNumber) {
    return d.fail("failed to match magic number");
  }

  if (!d.readFixedU32(&u32) || u32 != EncodingVersion) {
    return d.failf("binary version 0x%" PRIx32
                   " does not match expected version 0x%" PRIx32,
                   u32, EncodingVersion);
  }

  return true;
}

enum class TypeState { None, Gc, ForwardGc, Func };

using TypeStateVector = Vector<TypeState, 0, SystemAllocPolicy>;

template <class T>
static bool ValidateTypeState(Decoder& d, TypeStateVector* typeState, T type) {
  if (!type.isTypeIndex()) {
    return true;
  }

  uint32_t refTypeIndex = type.refType().typeIndex();
  switch ((*typeState)[refTypeIndex]) {
    case TypeState::None:
      (*typeState)[refTypeIndex] = TypeState::ForwardGc;
      break;
    case TypeState::Gc:
    case TypeState::ForwardGc:
      break;
    case TypeState::Func:
      return d.fail("ref does not reference a gc type");
  }
  return true;
}

#ifdef WASM_PRIVATE_REFTYPES
static bool FuncTypeIsJSCompatible(Decoder& d, const FuncType& ft) {
  if (ft.exposesTypeIndex()) {
    return d.fail("cannot expose indexed reference type");
  }
  return true;
}
#endif

static bool DecodeTypeVector(Decoder& d, ModuleEnvironment* env,
                             TypeStateVector* typeState, uint32_t count,
                             ValTypeVector* types) {
  if (!types->resize(count)) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!d.readValType(env->types.length(), env->features, &(*types)[i])) {
      return false;
    }
    if (!ValidateTypeState(d, typeState, (*types)[i])) {
      return false;
    }
  }
  return true;
}

static bool DecodeFuncType(Decoder& d, ModuleEnvironment* env,
                           TypeStateVector* typeState, uint32_t typeIndex) {
  uint32_t numArgs;
  if (!d.readVarU32(&numArgs)) {
    return d.fail("bad number of function args");
  }
  if (numArgs > MaxParams) {
    return d.fail("too many arguments in signature");
  }
  ValTypeVector args;
  if (!DecodeTypeVector(d, env, typeState, numArgs, &args)) {
    return false;
  }

  uint32_t numResults;
  if (!d.readVarU32(&numResults)) {
    return d.fail("bad number of function returns");
  }
  if (numResults > MaxResults) {
    return d.fail("too many returns in signature");
  }
  ValTypeVector results;
  if (!DecodeTypeVector(d, env, typeState, numResults, &results)) {
    return false;
  }

  if ((*typeState)[typeIndex] != TypeState::None) {
    return d.fail("function type entry referenced as gc");
  }

  env->types[typeIndex] =
      TypeDef(FuncType(std::move(args), std::move(results)));
  (*typeState)[typeIndex] = TypeState::Func;

  return true;
}

static bool DecodeStructType(Decoder& d, ModuleEnvironment* env,
                             TypeStateVector* typeState, uint32_t typeIndex) {
  if (!env->gcEnabled()) {
    return d.fail("Structure types not enabled");
  }

  if ((*typeState)[typeIndex] != TypeState::None &&
      (*typeState)[typeIndex] != TypeState::ForwardGc) {
    return d.fail("gc type entry referenced as function");
  }

  uint32_t numFields;
  if (!d.readVarU32(&numFields)) {
    return d.fail("Bad number of fields");
  }

  if (numFields > MaxStructFields) {
    return d.fail("too many fields in struct");
  }

  StructFieldVector fields;
  if (!fields.resize(numFields)) {
    return false;
  }

  for (uint32_t i = 0; i < numFields; i++) {
    if (!d.readPackedType(env->types.length(), env->features,
                          &fields[i].type)) {
      return false;
    }

    uint8_t flags;
    if (!d.readFixedU8(&flags)) {
      return d.fail("expected flag");
    }
    if ((flags & ~uint8_t(FieldFlags::AllowedMask)) != 0) {
      return d.fail("garbage flag bits");
    }
    fields[i].isMutable = flags & uint8_t(FieldFlags::Mutable);

    if (!ValidateTypeState(d, typeState, fields[i].type)) {
      return false;
    }
  }

  StructType structType = StructType(std::move(fields));

  if (!structType.computeLayout()) {
    return d.fail("Struct type too large");
  }

  env->types[typeIndex] = TypeDef(std::move(structType));
  (*typeState)[typeIndex] = TypeState::Gc;

  return true;
}

static bool DecodeArrayType(Decoder& d, ModuleEnvironment* env,
                            TypeStateVector* typeState, uint32_t typeIndex) {
  if (!env->gcEnabled()) {
    return d.fail("gc types not enabled");
  }

  if ((*typeState)[typeIndex] != TypeState::None &&
      (*typeState)[typeIndex] != TypeState::ForwardGc) {
    return d.fail("gc type entry referenced as function");
  }

  FieldType elementType;
  if (!d.readFieldType(env->types.length(), env->features, &elementType)) {
    return false;
  }

  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected flag");
  }
  if ((flags & ~uint8_t(FieldFlags::AllowedMask)) != 0) {
    return d.fail("garbage flag bits");
  }
  bool isMutable = flags & uint8_t(FieldFlags::Mutable);

  if (!ValidateTypeState(d, typeState, elementType)) {
    return false;
  }

  env->types[typeIndex] = TypeDef(ArrayType(elementType, isMutable));
  (*typeState)[typeIndex] = TypeState::Gc;

  return true;
}

static bool DecodeTypeSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Type, env, &range, "type")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numTypes;
  if (!d.readVarU32(&numTypes)) {
    return d.fail("expected number of types");
  }

  if (numTypes > MaxTypes) {
    return d.fail("too many types");
  }

  if (!env->types.resize(numTypes) || !env->typeIds.resize(numTypes)) {
    return false;
  }

  TypeStateVector typeState;
  if (!typeState.appendN(TypeState::None, numTypes)) {
    return false;
  }

  for (uint32_t typeIndex = 0; typeIndex < numTypes; typeIndex++) {
    uint8_t form;
    if (!d.readFixedU8(&form)) {
      return d.fail("expected type form");
    }

    switch (form) {
      case uint8_t(TypeCode::Func):
        if (!DecodeFuncType(d, env, &typeState, typeIndex)) {
          return false;
        }
        break;
      case uint8_t(TypeCode::Struct):
        if (!DecodeStructType(d, env, &typeState, typeIndex)) {
          return false;
        }
        break;
      case uint8_t(TypeCode::Array):
        if (!DecodeArrayType(d, env, &typeState, typeIndex)) {
          return false;
        }
        break;
      default:
        return d.fail("expected type form");
    }
  }

  return d.finishSection(*range, "type");
}

static UniqueChars DecodeName(Decoder& d) {
  uint32_t numBytes;
  if (!d.readVarU32(&numBytes)) {
    return nullptr;
  }

  if (numBytes > MaxStringBytes) {
    return nullptr;
  }

  const uint8_t* bytes;
  if (!d.readBytes(numBytes, &bytes)) {
    return nullptr;
  }

  if (!IsUtf8(AsChars(Span(bytes, numBytes)))) {
    return nullptr;
  }

  UniqueChars name(js_pod_malloc<char>(numBytes + 1));
  if (!name) {
    return nullptr;
  }

  memcpy(name.get(), bytes, numBytes);
  name[numBytes] = '\0';

  return name;
}

static bool DecodeFuncTypeIndex(Decoder& d, const TypeContext& types,
                                uint32_t* funcTypeIndex) {
  if (!d.readVarU32(funcTypeIndex)) {
    return d.fail("expected signature index");
  }

  if (*funcTypeIndex >= types.length()) {
    return d.fail("signature index out of range");
  }

  const TypeDef& def = types[*funcTypeIndex];

  if (!def.isFuncType()) {
    return d.fail("signature index references non-signature");
  }

  return true;
}

static bool DecodeLimits(Decoder& d, Limits* limits,
                         Shareable allowShared = Shareable::False) {
  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected flags");
  }

  uint8_t mask = allowShared == Shareable::True
                     ? uint8_t(MemoryMasks::AllowShared)
                     : uint8_t(MemoryMasks::AllowUnshared);

  if (flags & ~uint8_t(mask)) {
    return d.failf("unexpected bits set in flags: %" PRIu32,
                   uint32_t(flags & ~uint8_t(mask)));
  }

  uint32_t initial;
  if (!d.readVarU32(&initial)) {
    return d.fail("expected initial length");
  }
  limits->initial = initial;

  if (flags & uint8_t(MemoryTableFlags::HasMaximum)) {
    uint32_t maximum;
    if (!d.readVarU32(&maximum)) {
      return d.fail("expected maximum length");
    }

    if (limits->initial > maximum) {
      return d.failf(
          "memory size minimum must not be greater than maximum; "
          "maximum length %" PRIu32 " is less than initial length %" PRIu64,
          maximum, limits->initial);
    }

    limits->maximum.emplace(uint64_t(maximum));
  }

  limits->shared = Shareable::False;

  if (allowShared == Shareable::True) {
    if ((flags & uint8_t(MemoryTableFlags::IsShared)) &&
        !(flags & uint8_t(MemoryTableFlags::HasMaximum))) {
      return d.fail("maximum length required for shared memory");
    }

    limits->shared = (flags & uint8_t(MemoryTableFlags::IsShared))
                         ? Shareable::True
                         : Shareable::False;
  }

  return true;
}

static bool DecodeTableTypeAndLimits(Decoder& d, const FeatureArgs& features,
                                     const TypeContext& types,
                                     TableDescVector* tables) {
  RefType tableElemType;
  if (!d.readRefType(types, features, &tableElemType)) {
    return false;
  }
  if (!tableElemType.isNullable()) {
    return d.fail("non-nullable references not supported in tables");
  }

  Limits limits;
  if (!DecodeLimits(d, &limits)) {
    return false;
  }

  // If there's a maximum, check it is in range.  The check to exclude
  // initial > maximum is carried out by the DecodeLimits call above, so
  // we don't repeat it here.
  if (limits.initial > MaxTableLimitField ||
      ((limits.maximum.isSome() &&
        limits.maximum.value() > MaxTableLimitField))) {
    return d.fail("too many table elements");
  }

  if (tables->length() >= MaxTables) {
    return d.fail("too many tables");
  }

  // The rest of the runtime expects table limits to be within a 32-bit range.
  static_assert(MaxTableLimitField <= UINT32_MAX, "invariant");
  uint32_t initialLength = uint32_t(limits.initial);
  Maybe<uint32_t> maximumLength;
  if (limits.maximum) {
    maximumLength = Some(uint32_t(*limits.maximum));
  }

  return tables->emplaceBack(tableElemType, initialLength, maximumLength,
                             /* isAsmJS */ false);
}

static bool GlobalIsJSCompatible(Decoder& d, ValType type) {
  switch (type.kind()) {
    case ValType::I32:
    case ValType::F32:
    case ValType::F64:
    case ValType::I64:
    case ValType::V128:
      break;
    case ValType::Ref:
      switch (type.refTypeKind()) {
        case RefType::Func:
        case RefType::Extern:
        case RefType::Eq:
          break;
        case RefType::TypeIndex:
#ifdef WASM_PRIVATE_REFTYPES
          return d.fail("cannot expose indexed reference type");
#else
          break;
#endif
        default:
          return d.fail("unexpected variable type in global import/export");
      }
      break;
    default:
      return d.fail("unexpected variable type in global import/export");
  }

  return true;
}

static bool DecodeGlobalType(Decoder& d, const TypeContext& types,
                             const FeatureArgs& features, ValType* type,
                             bool* isMutable) {
  if (!d.readValType(types, features, type)) {
    return d.fail("expected global type");
  }

  if (type->isReference() && !type->isNullable()) {
    return d.fail("non-nullable references not supported in globals");
  }

  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected global flags");
  }

  if (flags & ~uint8_t(GlobalTypeImmediate::AllowedMask)) {
    return d.fail("unexpected bits set in global flags");
  }

  *isMutable = flags & uint8_t(GlobalTypeImmediate::IsMutable);
  return true;
}

static bool DecodeMemoryLimits(Decoder& d, ModuleEnvironment* env) {
  if (env->usesMemory()) {
    return d.fail("already have default memory");
  }

  Limits limits;
  if (!DecodeLimits(d, &limits, Shareable::True)) {
    return false;
  }

  if (limits.initial > MaxMemory32LimitField) {
    return d.fail("initial memory size too big");
  }

  if (limits.maximum && *limits.maximum > MaxMemory32LimitField) {
    return d.fail("maximum memory size too big");
  }

  if (limits.shared == Shareable::True &&
      env->sharedMemoryEnabled() == Shareable::False) {
    return d.fail("shared memory is disabled");
  }

  env->memory = Some(MemoryDesc(MemoryKind::Memory32, limits));
  return true;
}

#ifdef ENABLE_WASM_EXCEPTIONS
static bool EventIsJSCompatible(Decoder& d, const ValTypeVector& type) {
  for (auto t : type) {
    if (t.isTypeIndex()) {
      return d.fail("cannot expose indexed reference type");
    }
  }

  return true;
}

static bool DecodeEvent(Decoder& d, ModuleEnvironment* env,
                        EventKind* eventKind, uint32_t* funcTypeIndex) {
  uint32_t eventCode;
  if (!d.readVarU32(&eventCode)) {
    return d.fail("expected event kind");
  }

  if (EventKind(eventCode) != EventKind::Exception) {
    return d.fail("illegal event kind");
  }
  *eventKind = EventKind(eventCode);

  if (!d.readVarU32(funcTypeIndex)) {
    return d.fail("expected function index in event");
  }
  if (*funcTypeIndex >= env->numTypes()) {
    return d.fail("function type index in event out of bounds");
  }
  if (!env->types[*funcTypeIndex].isFuncType()) {
    return d.fail("function type index must index a function type");
  }
  if (env->types[*funcTypeIndex].funcType().results().length() != 0) {
    return d.fail("exception function types must not return anything");
  }
  return true;
}
#endif

struct CStringPair {
  const char* first;
  const char* second;

  CStringPair(const char* first, const char* second)
      : first(first), second(second) {}

  using Key = CStringPair;
  using Lookup = CStringPair;

  static mozilla::HashNumber hash(const Lookup& l) {
    return mozilla::AddToHash(mozilla::HashString(l.first),
                              mozilla::HashString(l.second));
  }
  static bool match(const Key& k, const Lookup& l) {
    return !strcmp(k.first, l.first) && !strcmp(k.second, l.second);
  }
};

using CStringPairSet = HashSet<CStringPair, CStringPair, SystemAllocPolicy>;

static bool DecodeImport(Decoder& d, ModuleEnvironment* env,
                         CStringPairSet* dupSet) {
  UniqueChars moduleName = DecodeName(d);
  if (!moduleName) {
    return d.fail("expected valid import module name");
  }

  UniqueChars funcName = DecodeName(d);
  if (!funcName) {
    return d.fail("expected valid import func name");
  }

  // It is valid to store raw pointers in dupSet because moduleName and funcName
  // become owned by env->imports on all non-error paths, outliving dupSet.
  CStringPair pair(moduleName.get(), funcName.get());
  CStringPairSet::AddPtr p = dupSet->lookupForAdd(pair);
  if (p) {
    env->usesDuplicateImports = true;
  } else if (!dupSet->add(p, pair)) {
    return false;
  }

  uint8_t rawImportKind;
  if (!d.readFixedU8(&rawImportKind)) {
    return d.fail("failed to read import kind");
  }

  DefinitionKind importKind = DefinitionKind(rawImportKind);

  switch (importKind) {
    case DefinitionKind::Function: {
      uint32_t funcTypeIndex;
      if (!DecodeFuncTypeIndex(d, env->types, &funcTypeIndex)) {
        return false;
      }
#ifdef WASM_PRIVATE_REFTYPES
      if (!FuncTypeIsJSCompatible(d, env->types.funcType(funcTypeIndex))) {
        return false;
      }
#endif
      if (!env->funcs.append(FuncDesc(&env->types.funcType(funcTypeIndex),
                                      &env->typeIds[funcTypeIndex],
                                      funcTypeIndex))) {
        return false;
      }
      if (env->funcs.length() > MaxFuncs) {
        return d.fail("too many functions");
      }
      break;
    }
    case DefinitionKind::Table: {
      if (!DecodeTableTypeAndLimits(d, env->features, env->types,
                                    &env->tables)) {
        return false;
      }
      env->tables.back().importedOrExported = true;
      break;
    }
    case DefinitionKind::Memory: {
      if (!DecodeMemoryLimits(d, env)) {
        return false;
      }
      break;
    }
    case DefinitionKind::Global: {
      ValType type;
      bool isMutable;
      if (!DecodeGlobalType(d, env->types, env->features, &type, &isMutable)) {
        return false;
      }
      if (!GlobalIsJSCompatible(d, type)) {
        return false;
      }
      if (!env->globals.append(
              GlobalDesc(type, isMutable, env->globals.length()))) {
        return false;
      }
      if (env->globals.length() > MaxGlobals) {
        return d.fail("too many globals");
      }
      break;
    }
#ifdef ENABLE_WASM_EXCEPTIONS
    case DefinitionKind::Event: {
      EventKind eventKind;
      uint32_t funcTypeIndex;
      if (!DecodeEvent(d, env, &eventKind, &funcTypeIndex)) {
        return false;
      }
      const ValTypeVector& args = env->types[funcTypeIndex].funcType().args();
#  ifdef WASM_PRIVATE_REFTYPES
      if (!EventIsJSCompatible(d, args)) {
        return false;
      }
#  endif
      ValTypeVector eventArgs;
      if (!eventArgs.appendAll(args)) {
        return false;
      }
      if (!env->events.emplaceBack(eventKind, std::move(eventArgs))) {
        return false;
      }
      if (env->events.length() > MaxEvents) {
        return d.fail("too many events");
      }
      break;
    }
#endif
    default:
      return d.fail("unsupported import kind");
  }

  return env->imports.emplaceBack(std::move(moduleName), std::move(funcName),
                                  importKind);
}

static bool DecodeImportSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Import, env, &range, "import")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numImports;
  if (!d.readVarU32(&numImports)) {
    return d.fail("failed to read number of imports");
  }

  if (numImports > MaxImports) {
    return d.fail("too many imports");
  }

  CStringPairSet dupSet;
  for (uint32_t i = 0; i < numImports; i++) {
    if (!DecodeImport(d, env, &dupSet)) {
      return false;
    }
  }

  if (!d.finishSection(*range, "import")) {
    return false;
  }

  // The global data offsets will be filled in by ModuleGenerator::init.
  if (!env->funcImportGlobalDataOffsets.resize(env->funcs.length())) {
    return false;
  }

  return true;
}

static bool DecodeFunctionSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Function, env, &range, "function")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of function definitions");
  }

  CheckedInt<uint32_t> numFuncs = env->funcs.length();
  numFuncs += numDefs;
  if (!numFuncs.isValid() || numFuncs.value() > MaxFuncs) {
    return d.fail("too many functions");
  }

  if (!env->funcs.reserve(numFuncs.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    uint32_t funcTypeIndex;
    if (!DecodeFuncTypeIndex(d, env->types, &funcTypeIndex)) {
      return false;
    }
    env->funcs.infallibleAppend(FuncDesc(&env->types.funcType(funcTypeIndex),
                                         &env->typeIds[funcTypeIndex],
                                         funcTypeIndex));
  }

  return d.finishSection(*range, "function");
}

static bool DecodeTableSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Table, env, &range, "table")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numTables;
  if (!d.readVarU32(&numTables)) {
    return d.fail("failed to read number of tables");
  }

  for (uint32_t i = 0; i < numTables; ++i) {
    if (!DecodeTableTypeAndLimits(d, env->features, env->types, &env->tables)) {
      return false;
    }
  }

  return d.finishSection(*range, "table");
}

static bool DecodeMemorySection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Memory, env, &range, "memory")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numMemories;
  if (!d.readVarU32(&numMemories)) {
    return d.fail("failed to read number of memories");
  }

  if (numMemories > 1) {
    return d.fail("the number of memories must be at most one");
  }

  for (uint32_t i = 0; i < numMemories; ++i) {
    if (!DecodeMemoryLimits(d, env)) {
      return false;
    }
  }

  return d.finishSection(*range, "memory");
}

static bool DecodeGlobalSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Global, env, &range, "global")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of globals");
  }

  CheckedInt<uint32_t> numGlobals = env->globals.length();
  numGlobals += numDefs;
  if (!numGlobals.isValid() || numGlobals.value() > MaxGlobals) {
    return d.fail("too many globals");
  }

  if (!env->globals.reserve(numGlobals.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    ValType type;
    bool isMutable;
    if (!DecodeGlobalType(d, env->types, env->features, &type, &isMutable)) {
      return false;
    }

    InitExpr initializer;
    if (!InitExpr::decodeAndValidate(d, env, type, &initializer)) {
      return false;
    }

    env->globals.infallibleAppend(
        GlobalDesc(std::move(initializer), isMutable));
  }

  return d.finishSection(*range, "global");
}

#ifdef ENABLE_WASM_EXCEPTIONS
static bool DecodeEventSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Event, env, &range, "event")) {
    return false;
  }
  if (!range) {
    return true;
  }

  if (!env->exceptionsEnabled()) {
    return d.fail("exceptions not enabled");
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of events");
  }

  CheckedInt<uint32_t> numEvents = env->events.length();
  numEvents += numDefs;
  if (!numEvents.isValid() || numEvents.value() > MaxEvents) {
    return d.fail("too many events");
  }

  if (!env->events.reserve(numEvents.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    EventKind eventKind;
    uint32_t funcTypeIndex;
    if (!DecodeEvent(d, env, &eventKind, &funcTypeIndex)) {
      return false;
    }
    const ValTypeVector& args = env->types[funcTypeIndex].funcType().args();
    ValTypeVector eventArgs;
    if (!eventArgs.appendAll(args)) {
      return false;
    }
    env->events.infallibleEmplaceBack(eventKind, std::move(eventArgs));
  }

  return d.finishSection(*range, "event");
}
#endif

using CStringSet =
    HashSet<const char*, mozilla::CStringHasher, SystemAllocPolicy>;

static UniqueChars DecodeExportName(Decoder& d, CStringSet* dupSet) {
  UniqueChars exportName = DecodeName(d);
  if (!exportName) {
    d.fail("expected valid export name");
    return nullptr;
  }

  CStringSet::AddPtr p = dupSet->lookupForAdd(exportName.get());
  if (p) {
    d.fail("duplicate export");
    return nullptr;
  }

  if (!dupSet->add(p, exportName.get())) {
    return nullptr;
  }

  return exportName;
}

static bool DecodeExport(Decoder& d, ModuleEnvironment* env,
                         CStringSet* dupSet) {
  UniqueChars fieldName = DecodeExportName(d, dupSet);
  if (!fieldName) {
    return false;
  }

  uint8_t exportKind;
  if (!d.readFixedU8(&exportKind)) {
    return d.fail("failed to read export kind");
  }

  switch (DefinitionKind(exportKind)) {
    case DefinitionKind::Function: {
      uint32_t funcIndex;
      if (!d.readVarU32(&funcIndex)) {
        return d.fail("expected function index");
      }

      if (funcIndex >= env->numFuncs()) {
        return d.fail("exported function index out of bounds");
      }
#ifdef WASM_PRIVATE_REFTYPES
      if (!FuncTypeIsJSCompatible(d, *env->funcs[funcIndex].type)) {
        return false;
      }
#endif

      env->declareFuncExported(funcIndex, /* eager */ true,
                               /* canRefFunc */ true);
      return env->exports.emplaceBack(std::move(fieldName), funcIndex,
                                      DefinitionKind::Function);
    }
    case DefinitionKind::Table: {
      uint32_t tableIndex;
      if (!d.readVarU32(&tableIndex)) {
        return d.fail("expected table index");
      }

      if (tableIndex >= env->tables.length()) {
        return d.fail("exported table index out of bounds");
      }
      env->tables[tableIndex].importedOrExported = true;
      return env->exports.emplaceBack(std::move(fieldName), tableIndex,
                                      DefinitionKind::Table);
    }
    case DefinitionKind::Memory: {
      uint32_t memoryIndex;
      if (!d.readVarU32(&memoryIndex)) {
        return d.fail("expected memory index");
      }

      if (memoryIndex > 0 || !env->usesMemory()) {
        return d.fail("exported memory index out of bounds");
      }

      return env->exports.emplaceBack(std::move(fieldName),
                                      DefinitionKind::Memory);
    }
    case DefinitionKind::Global: {
      uint32_t globalIndex;
      if (!d.readVarU32(&globalIndex)) {
        return d.fail("expected global index");
      }

      if (globalIndex >= env->globals.length()) {
        return d.fail("exported global index out of bounds");
      }

      GlobalDesc* global = &env->globals[globalIndex];
      global->setIsExport();
      if (!GlobalIsJSCompatible(d, global->type())) {
        return false;
      }

      return env->exports.emplaceBack(std::move(fieldName), globalIndex,
                                      DefinitionKind::Global);
    }
#ifdef ENABLE_WASM_EXCEPTIONS
    case DefinitionKind::Event: {
      uint32_t eventIndex;
      if (!d.readVarU32(&eventIndex)) {
        return d.fail("expected event index");
      }
      if (eventIndex >= env->events.length()) {
        return d.fail("exported event index out of bounds");
      }

#  ifdef WASM_PRIVATE_REFTYPES
      if (!EventIsJSCompatible(d, env->events[eventIndex].type)) {
        return false;
      }
#  endif

      env->events[eventIndex].isExport = true;
      return env->exports.emplaceBack(std::move(fieldName), eventIndex,
                                      DefinitionKind::Event);
    }
#endif
    default:
      return d.fail("unexpected export kind");
  }

  MOZ_CRASH("unreachable");
}

static bool DecodeExportSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Export, env, &range, "export")) {
    return false;
  }
  if (!range) {
    return true;
  }

  CStringSet dupSet;

  uint32_t numExports;
  if (!d.readVarU32(&numExports)) {
    return d.fail("failed to read number of exports");
  }

  if (numExports > MaxExports) {
    return d.fail("too many exports");
  }

  for (uint32_t i = 0; i < numExports; i++) {
    if (!DecodeExport(d, env, &dupSet)) {
      return false;
    }
  }

  return d.finishSection(*range, "export");
}

static bool DecodeStartSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Start, env, &range, "start")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t funcIndex;
  if (!d.readVarU32(&funcIndex)) {
    return d.fail("failed to read start func index");
  }

  if (funcIndex >= env->numFuncs()) {
    return d.fail("unknown start function");
  }

  const FuncType& funcType = *env->funcs[funcIndex].type;
  if (funcType.results().length() > 0) {
    return d.fail("start function must not return anything");
  }

  if (funcType.args().length()) {
    return d.fail("start function must be nullary");
  }

  env->declareFuncExported(funcIndex, /* eager */ true, /* canFuncRef */ false);
  env->startFuncIndex = Some(funcIndex);

  return d.finishSection(*range, "start");
}

static inline ElemSegment::Kind NormalizeElemSegmentKind(
    ElemSegmentKind decodedKind) {
  switch (decodedKind) {
    case ElemSegmentKind::Active:
    case ElemSegmentKind::ActiveWithTableIndex: {
      return ElemSegment::Kind::Active;
    }
    case ElemSegmentKind::Passive: {
      return ElemSegment::Kind::Passive;
    }
    case ElemSegmentKind::Declared: {
      return ElemSegment::Kind::Declared;
    }
  }
  MOZ_CRASH("unexpected elem segment kind");
}

static bool DecodeElemSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Elem, env, &range, "elem")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numSegments;
  if (!d.readVarU32(&numSegments)) {
    return d.fail("failed to read number of elem segments");
  }

  if (numSegments > MaxElemSegments) {
    return d.fail("too many elem segments");
  }

  if (!env->elemSegments.reserve(numSegments)) {
    return false;
  }

  for (uint32_t i = 0; i < numSegments; i++) {
    uint32_t segmentFlags;
    if (!d.readVarU32(&segmentFlags)) {
      return d.fail("expected elem segment flags field");
    }

    Maybe<ElemSegmentFlags> flags = ElemSegmentFlags::construct(segmentFlags);
    if (!flags) {
      return d.fail("invalid elem segment flags field");
    }

    MutableElemSegment seg = js_new<ElemSegment>();
    if (!seg) {
      return false;
    }

    ElemSegmentKind kind = flags->kind();
    seg->kind = NormalizeElemSegmentKind(kind);

    if (kind == ElemSegmentKind::Active ||
        kind == ElemSegmentKind::ActiveWithTableIndex) {
      if (env->tables.length() == 0) {
        return d.fail("active elem segment requires a table");
      }

      uint32_t tableIndex = 0;
      if (kind == ElemSegmentKind::ActiveWithTableIndex &&
          !d.readVarU32(&tableIndex)) {
        return d.fail("expected table index");
      }
      if (tableIndex >= env->tables.length()) {
        return d.fail("table index out of range for element segment");
      }
      seg->tableIndex = tableIndex;

      InitExpr offset;
      if (!InitExpr::decodeAndValidate(d, env, ValType::I32, &offset)) {
        return false;
      }
      seg->offsetIfActive.emplace(std::move(offset));
    } else {
      // Too many bugs result from keeping this value zero.  For passive
      // or declared segments, there really is no table index, and we should
      // never touch the field.
      MOZ_ASSERT(kind == ElemSegmentKind::Passive ||
                 kind == ElemSegmentKind::Declared);
      seg->tableIndex = (uint32_t)-1;
    }

    ElemSegmentPayload payload = flags->payload();
    RefType elemType;

    // `ActiveWithTableIndex`, `Declared`, and `Passive` element segments encode
    // the type or definition kind of the payload. `Active` element segments are
    // restricted to MVP behavior, which assumes only function indices.
    if (kind == ElemSegmentKind::Active) {
      elemType = RefType::func();
    } else {
      switch (payload) {
        case ElemSegmentPayload::ElemExpression: {
          if (!d.readRefType(env->types, env->features, &elemType)) {
            return false;
          }
          break;
        }
        case ElemSegmentPayload::ExternIndex: {
          uint8_t form;
          if (!d.readFixedU8(&form)) {
            return d.fail("expected type or extern kind");
          }

          if (form != uint8_t(DefinitionKind::Function)) {
            return d.fail(
                "segments with extern indices can only contain function "
                "references");
          }
          elemType = RefType::func();
        }
      }
    }

    // Check constraints on the element type.
    switch (kind) {
      case ElemSegmentKind::Active:
      case ElemSegmentKind::ActiveWithTableIndex: {
        RefType tblElemType = env->tables[seg->tableIndex].elemType;
        TypeCache cache;
        if (!CheckIsSubtypeOf(d, *env, d.currentOffset(), ValType(elemType),
                              ValType(tblElemType), &cache)) {
          return false;
        }
        break;
      }
      case ElemSegmentKind::Declared:
      case ElemSegmentKind::Passive: {
        // Passive segment element types are checked when used with a
        // `table.init` instruction.
        break;
      }
    }
    seg->elemType = elemType;

    uint32_t numElems;
    if (!d.readVarU32(&numElems)) {
      return d.fail("expected segment size");
    }

    if (numElems > MaxElemSegmentLength) {
      return d.fail("too many table elements");
    }

    if (!seg->elemFuncIndices.reserve(numElems)) {
      return false;
    }

#ifdef WASM_PRIVATE_REFTYPES
    // We assume that passive or declared segments may be applied to external
    // tables. We can do slightly better: if there are no external tables in
    // the module then we don't need to worry about passive or declared
    // segments either. But this is a temporary restriction.
    bool exportedTable = kind == ElemSegmentKind::Passive ||
                         kind == ElemSegmentKind::Declared ||
                         env->tables[seg->tableIndex].importedOrExported;
#endif
    bool isAsmJS = seg->active() && env->tables[seg->tableIndex].isAsmJS;

    // For passive segments we should use InitExpr but we don't really want to
    // generalize the ElemSection data structure yet, so instead read the
    // required Ref.Func and End here.

    TypeCache cache;
    for (uint32_t i = 0; i < numElems; i++) {
      bool needIndex = true;

      if (payload == ElemSegmentPayload::ElemExpression) {
        OpBytes op;
        if (!d.readOp(&op)) {
          return d.fail("failed to read initializer operation");
        }

        RefType initType = RefType::extern_();
        switch (op.b0) {
          case uint16_t(Op::RefFunc):
            initType = RefType::func();
            break;
          case uint16_t(Op::RefNull):
            if (!d.readHeapType(env->types, env->features, true, &initType)) {
              return false;
            }
            needIndex = false;
            break;
          default:
            return d.fail("failed to read initializer operation");
        }
        if (!CheckIsSubtypeOf(d, *env, d.currentOffset(), ValType(initType),
                              ValType(elemType), &cache)) {
          return false;
        }
      }

      uint32_t funcIndex = NullFuncIndex;
      if (needIndex) {
        if (!d.readVarU32(&funcIndex)) {
          return d.fail("failed to read element function index");
        }
        if (funcIndex >= env->numFuncs()) {
          return d.fail("table element out of range");
        }
#ifdef WASM_PRIVATE_REFTYPES
        if (exportedTable &&
            !FuncTypeIsJSCompatible(d, *env->funcs[funcIndex].type)) {
          return false;
        }
#endif
      }

      if (payload == ElemSegmentPayload::ElemExpression) {
        OpBytes end;
        if (!d.readOp(&end) || end.b0 != uint16_t(Op::End)) {
          return d.fail("failed to read end of initializer expression");
        }
      }

      seg->elemFuncIndices.infallibleAppend(funcIndex);
      if (funcIndex != NullFuncIndex && !isAsmJS) {
        env->declareFuncExported(funcIndex, /* eager */ false,
                                 /* canRefFunc */ true);
      }
    }

    env->elemSegments.infallibleAppend(std::move(seg));
  }

  return d.finishSection(*range, "elem");
}

static bool DecodeDataCountSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::DataCount, env, &range, "datacount")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t dataCount;
  if (!d.readVarU32(&dataCount)) {
    return d.fail("expected data segment count");
  }

  env->dataCount.emplace(dataCount);

  return d.finishSection(*range, "datacount");
}

bool wasm::StartsCodeSection(const uint8_t* begin, const uint8_t* end,
                             SectionRange* codeSection) {
  UniqueChars unused;
  Decoder d(begin, end, 0, &unused);

  if (!DecodePreamble(d)) {
    return false;
  }

  while (!d.done()) {
    uint8_t id;
    SectionRange range;
    if (!d.readSectionHeader(&id, &range)) {
      return false;
    }

    if (id == uint8_t(SectionId::Code)) {
      *codeSection = range;
      return true;
    }

    if (!d.readBytes(range.size)) {
      return false;
    }
  }

  return false;
}

bool wasm::DecodeModuleEnvironment(Decoder& d, ModuleEnvironment* env) {
  if (!DecodePreamble(d)) {
    return false;
  }

  if (!DecodeTypeSection(d, env)) {
    return false;
  }

  if (!DecodeImportSection(d, env)) {
    return false;
  }

  if (!DecodeFunctionSection(d, env)) {
    return false;
  }

  if (!DecodeTableSection(d, env)) {
    return false;
  }

  if (!DecodeMemorySection(d, env)) {
    return false;
  }

#ifdef ENABLE_WASM_EXCEPTIONS
  if (!DecodeEventSection(d, env)) {
    return false;
  }
#endif

  if (!DecodeGlobalSection(d, env)) {
    return false;
  }

  if (!DecodeExportSection(d, env)) {
    return false;
  }

  if (!DecodeStartSection(d, env)) {
    return false;
  }

  if (!DecodeElemSection(d, env)) {
    return false;
  }

  if (!DecodeDataCountSection(d, env)) {
    return false;
  }

  if (!d.startSection(SectionId::Code, env, &env->codeSection, "code")) {
    return false;
  }

  if (env->codeSection && env->codeSection->size > MaxCodeSectionBytes) {
    return d.fail("code section too big");
  }

  return true;
}

static bool DecodeFunctionBody(Decoder& d, const ModuleEnvironment& env,
                               uint32_t funcIndex) {
  uint32_t bodySize;
  if (!d.readVarU32(&bodySize)) {
    return d.fail("expected number of function body bytes");
  }

  if (bodySize > MaxFunctionBytes) {
    return d.fail("function body too big");
  }

  if (d.bytesRemain() < bodySize) {
    return d.fail("function body length too big");
  }

  return ValidateFunctionBody(env, funcIndex, bodySize, d);
}

static bool DecodeCodeSection(Decoder& d, ModuleEnvironment* env) {
  if (!env->codeSection) {
    if (env->numFuncDefs() != 0) {
      return d.fail("expected code section");
    }
    return true;
  }

  uint32_t numFuncDefs;
  if (!d.readVarU32(&numFuncDefs)) {
    return d.fail("expected function body count");
  }

  if (numFuncDefs != env->numFuncDefs()) {
    return d.fail(
        "function body count does not match function signature count");
  }

  for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
    if (!DecodeFunctionBody(d, *env, env->numFuncImports() + funcDefIndex)) {
      return false;
    }
  }

  return d.finishSection(*env->codeSection, "code");
}

static bool DecodeDataSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Data, env, &range, "data")) {
    return false;
  }
  if (!range) {
    if (env->dataCount.isSome() && *env->dataCount > 0) {
      return d.fail("number of data segments does not match declared count");
    }
    return true;
  }

  uint32_t numSegments;
  if (!d.readVarU32(&numSegments)) {
    return d.fail("failed to read number of data segments");
  }

  if (numSegments > MaxDataSegments) {
    return d.fail("too many data segments");
  }

  if (env->dataCount.isSome() && numSegments != *env->dataCount) {
    return d.fail("number of data segments does not match declared count");
  }

  for (uint32_t i = 0; i < numSegments; i++) {
    uint32_t initializerKindVal;
    if (!d.readVarU32(&initializerKindVal)) {
      return d.fail("expected data initializer-kind field");
    }

    switch (initializerKindVal) {
      case uint32_t(DataSegmentKind::Active):
      case uint32_t(DataSegmentKind::Passive):
      case uint32_t(DataSegmentKind::ActiveWithMemoryIndex):
        break;
      default:
        return d.fail("invalid data initializer-kind field");
    }

    DataSegmentKind initializerKind = DataSegmentKind(initializerKindVal);

    if (initializerKind != DataSegmentKind::Passive && !env->usesMemory()) {
      return d.fail("active data segment requires a memory section");
    }

    uint32_t memIndex = 0;
    if (initializerKind == DataSegmentKind::ActiveWithMemoryIndex) {
      if (!d.readVarU32(&memIndex)) {
        return d.fail("expected memory index");
      }
      if (memIndex > 0) {
        return d.fail("memory index must be zero");
      }
    }

    DataSegmentEnv seg;
    if (initializerKind == DataSegmentKind::Active ||
        initializerKind == DataSegmentKind::ActiveWithMemoryIndex) {
      InitExpr segOffset;
      if (!InitExpr::decodeAndValidate(d, env, ValType::I32, &segOffset)) {
        return false;
      }
      seg.offsetIfActive.emplace(std::move(segOffset));
    }

    if (!d.readVarU32(&seg.length)) {
      return d.fail("expected segment size");
    }

    if (seg.length > MaxDataSegmentLengthPages * PageSize) {
      return d.fail("segment size too big");
    }

    seg.bytecodeOffset = d.currentOffset();

    if (!d.readBytes(seg.length)) {
      return d.fail("data segment shorter than declared");
    }

    if (!env->dataSegments.append(std::move(seg))) {
      return false;
    }
  }

  return d.finishSection(*range, "data");
}

static bool DecodeModuleNameSubsection(Decoder& d,
                                       const CustomSectionEnv& nameSection,
                                       ModuleEnvironment* env) {
  Maybe<uint32_t> endOffset;
  if (!d.startNameSubsection(NameType::Module, &endOffset)) {
    return false;
  }
  if (!endOffset) {
    return true;
  }

  Name moduleName;
  if (!d.readVarU32(&moduleName.length)) {
    return d.fail("failed to read module name length");
  }

  MOZ_ASSERT(d.currentOffset() >= nameSection.payloadOffset);
  moduleName.offsetInNamePayload =
      d.currentOffset() - nameSection.payloadOffset;

  const uint8_t* bytes;
  if (!d.readBytes(moduleName.length, &bytes)) {
    return d.fail("failed to read module name bytes");
  }

  if (!d.finishNameSubsection(*endOffset)) {
    return false;
  }

  // Only save the module name if the whole subsection validates.
  env->moduleName.emplace(moduleName);
  return true;
}

static bool DecodeFunctionNameSubsection(Decoder& d,
                                         const CustomSectionEnv& nameSection,
                                         ModuleEnvironment* env) {
  Maybe<uint32_t> endOffset;
  if (!d.startNameSubsection(NameType::Function, &endOffset)) {
    return false;
  }
  if (!endOffset) {
    return true;
  }

  uint32_t nameCount = 0;
  if (!d.readVarU32(&nameCount) || nameCount > MaxFuncs) {
    return d.fail("bad function name count");
  }

  NameVector funcNames;

  for (uint32_t i = 0; i < nameCount; ++i) {
    uint32_t funcIndex = 0;
    if (!d.readVarU32(&funcIndex)) {
      return d.fail("unable to read function index");
    }

    // Names must refer to real functions and be given in ascending order.
    if (funcIndex >= env->numFuncs() || funcIndex < funcNames.length()) {
      return d.fail("invalid function index");
    }

    Name funcName;
    if (!d.readVarU32(&funcName.length) ||
        funcName.length > JS::MaxStringLength) {
      return d.fail("unable to read function name length");
    }

    if (!funcName.length) {
      continue;
    }

    if (!funcNames.resize(funcIndex + 1)) {
      return false;
    }

    MOZ_ASSERT(d.currentOffset() >= nameSection.payloadOffset);
    funcName.offsetInNamePayload =
        d.currentOffset() - nameSection.payloadOffset;

    if (!d.readBytes(funcName.length)) {
      return d.fail("unable to read function name bytes");
    }

    funcNames[funcIndex] = funcName;
  }

  if (!d.finishNameSubsection(*endOffset)) {
    return false;
  }

  // To encourage fully valid function names subsections; only save names if
  // the entire subsection decoded correctly.
  env->funcNames = std::move(funcNames);
  return true;
}

static bool DecodeNameSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startCustomSection(NameSectionName, env, &range)) {
    return false;
  }
  if (!range) {
    return true;
  }

  env->nameCustomSectionIndex = Some(env->customSections.length() - 1);
  const CustomSectionEnv& nameSection = env->customSections.back();

  // Once started, custom sections do not report validation errors.

  if (!DecodeModuleNameSubsection(d, nameSection, env)) {
    goto finish;
  }

  if (!DecodeFunctionNameSubsection(d, nameSection, env)) {
    goto finish;
  }

  while (d.currentOffset() < range->end()) {
    if (!d.skipNameSubsection()) {
      goto finish;
    }
  }

finish:
  d.finishCustomSection(NameSectionName, *range);
  return true;
}

bool wasm::DecodeModuleTail(Decoder& d, ModuleEnvironment* env) {
  if (!DecodeDataSection(d, env)) {
    return false;
  }

  if (!DecodeNameSection(d, env)) {
    return false;
  }

  while (!d.done()) {
    if (!d.skipCustomSection(env)) {
      if (d.resilientMode()) {
        d.clearError();
        return true;
      }
      return false;
    }
  }

  return true;
}

// Validate algorithm.

bool wasm::Validate(JSContext* cx, const ShareableBytes& bytecode,
                    const FeatureOptions& options, UniqueChars* error) {
  Decoder d(bytecode.bytes, 0, error);

  FeatureArgs features = FeatureArgs::build(cx, options);
  ModuleEnvironment env(features);
  if (!DecodeModuleEnvironment(d, &env)) {
    return false;
  }

  if (!DecodeCodeSection(d, &env)) {
    return false;
  }

  if (!DecodeModuleTail(d, &env)) {
    return false;
  }

  MOZ_ASSERT(!*error, "unreported error in decoding");
  return true;
}
