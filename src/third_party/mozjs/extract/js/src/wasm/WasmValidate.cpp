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
#include "mozilla/Span.h"
#include "mozilla/Utf8.h"

#include "js/Printf.h"
#include "js/String.h"  // JS::MaxStringLength
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmTypeDecls.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::AsChars;
using mozilla::CheckedInt;
using mozilla::CheckedInt32;
using mozilla::IsUtf8;
using mozilla::Span;

// Module environment helpers.

bool ModuleEnvironment::addDefinedFunc(
    ValTypeVector&& params, ValTypeVector&& results, bool declareForRef,
    Maybe<CacheableName>&& optionalExportedName) {
  uint32_t typeIndex = types->length();
  FuncType funcType(std::move(params), std::move(results));
  if (!types->addType(std::move(funcType))) {
    return false;
  }

  FuncDesc funcDesc = FuncDesc(&(*types)[typeIndex].funcType(), typeIndex);
  uint32_t funcIndex = funcs.length();
  if (!funcs.append(funcDesc)) {
    return false;
  }
  if (declareForRef) {
    declareFuncExported(funcIndex, true, true);
  }
  if (optionalExportedName.isSome()) {
    if (!exports.emplaceBack(std::move(optionalExportedName.ref()), funcIndex,
                             DefinitionKind::Function)) {
      return false;
    }
  }
  return true;
}

bool ModuleEnvironment::addImportedFunc(ValTypeVector&& params,
                                        ValTypeVector&& results,
                                        CacheableName&& importModName,
                                        CacheableName&& importFieldName) {
  MOZ_ASSERT(numFuncImports == funcs.length());
  if (!addDefinedFunc(std::move(params), std::move(results), false,
                      mozilla::Nothing())) {
    return false;
  }
  numFuncImports++;
  return imports.emplaceBack(std::move(importModName),
                             std::move(importFieldName),
                             DefinitionKind::Function);
}

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

bool wasm::DecodeLocalEntriesWithParams(Decoder& d,
                                        const ModuleEnvironment& env,
                                        uint32_t funcIndex,
                                        ValTypeVector* locals) {
  uint32_t numLocalEntries;
  if (!d.readVarU32(&numLocalEntries)) {
    return d.fail("failed to read number of local entries");
  }

  if (!locals->appendAll(env.funcs[funcIndex].type->args())) {
    return false;
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
    if (!d.readValType(*env.types, env.features, &type)) {
      return false;
    }

    if (!locals->appendN(type, count)) {
      return false;
    }
  }

  return true;
}

bool wasm::DecodeValidatedLocalEntries(const TypeContext& types, Decoder& d,
                                       ValTypeVector* locals) {
  uint32_t numLocalEntries;
  MOZ_ALWAYS_TRUE(d.readVarU32(&numLocalEntries));

  for (uint32_t i = 0; i < numLocalEntries; i++) {
    uint32_t count = d.uncheckedReadVarU32();
    MOZ_ASSERT(MaxLocals - locals->length() >= count);
    if (!locals->appendN(d.uncheckedReadValType(types), count)) {
      return false;
    }
  }

  return true;
}

bool wasm::CheckIsSubtypeOf(Decoder& d, const ModuleEnvironment& env,
                            size_t opcodeOffset, StorageType subType,
                            StorageType superType) {
  if (StorageType::isSubTypeOf(subType, superType)) {
    return true;
  }

  UniqueChars subText = ToString(subType, env.types);
  if (!subText) {
    return false;
  }

  UniqueChars superText = ToString(superType, env.types);
  if (!superText) {
    return false;
  }

  UniqueChars error(
      JS_smprintf("type mismatch: expression has type %s but expected %s",
                  subText.get(), superText.get()));
  if (!error) {
    return false;
  }

  return d.fail(opcodeOffset, error.get());
}

// Function body validation.

static bool DecodeFunctionBodyExprs(const ModuleEnvironment& env,
                                    uint32_t funcIndex,
                                    const ValTypeVector& locals,
                                    const uint8_t* bodyEnd, Decoder* d) {
  ValidatingOpIter iter(env, *d);

  if (!iter.startFunction(funcIndex, locals)) {
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
#ifdef ENABLE_WASM_TAIL_CALLS
      case uint16_t(Op::ReturnCall): {
        if (!env.tailCallsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedIndex;
        NothingVector unusedArgs{};
        CHECK(iter.readReturnCall(&unusedIndex, &unusedArgs));
      }
      case uint16_t(Op::ReturnCallIndirect): {
        if (!env.tailCallsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedIndex, unusedIndex2;
        NothingVector unusedArgs{};
        CHECK(iter.readReturnCallIndirect(&unusedIndex, &unusedIndex2, &nothing,
                                          &unusedArgs));
      }
#endif
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::CallRef): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        const FuncType* unusedType;
        NothingVector unusedArgs{};
        CHECK(iter.readCallRef(&unusedType, &nothing, &unusedArgs));
      }
#  ifdef ENABLE_WASM_TAIL_CALLS
      case uint16_t(Op::ReturnCallRef): {
        if (!env.gcEnabled() || !env.tailCallsEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        const FuncType* unusedType;
        NothingVector unusedArgs{};
        CHECK(iter.readReturnCallRef(&unusedType, &nothing, &unusedArgs));
      }
#  endif
#endif
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
      case uint16_t(Op::LocalGet): {
        uint32_t unused;
        CHECK(iter.readGetLocal(locals, &unused));
      }
      case uint16_t(Op::LocalSet): {
        uint32_t unused;
        CHECK(iter.readSetLocal(locals, &unused, &nothing));
      }
      case uint16_t(Op::LocalTee): {
        uint32_t unused;
        CHECK(iter.readTeeLocal(locals, &unused, &nothing));
      }
      case uint16_t(Op::GlobalGet): {
        uint32_t unused;
        CHECK(iter.readGetGlobal(&unused));
      }
      case uint16_t(Op::GlobalSet): {
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
      case uint16_t(Op::I32TruncF32S):
      case uint16_t(Op::I32TruncF32U):
      case uint16_t(Op::I32ReinterpretF32):
        CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
      case uint16_t(Op::I32TruncF64S):
      case uint16_t(Op::I32TruncF64U):
        CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
      case uint16_t(Op::I64ExtendI32S):
      case uint16_t(Op::I64ExtendI32U):
        CHECK(iter.readConversion(ValType::I32, ValType::I64, &nothing));
      case uint16_t(Op::I64TruncF32S):
      case uint16_t(Op::I64TruncF32U):
        CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
      case uint16_t(Op::I64TruncF64S):
      case uint16_t(Op::I64TruncF64U):
      case uint16_t(Op::I64ReinterpretF64):
        CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
      case uint16_t(Op::F32ConvertI32S):
      case uint16_t(Op::F32ConvertI32U):
      case uint16_t(Op::F32ReinterpretI32):
        CHECK(iter.readConversion(ValType::I32, ValType::F32, &nothing));
      case uint16_t(Op::F32ConvertI64S):
      case uint16_t(Op::F32ConvertI64U):
        CHECK(iter.readConversion(ValType::I64, ValType::F32, &nothing));
      case uint16_t(Op::F32DemoteF64):
        CHECK(iter.readConversion(ValType::F64, ValType::F32, &nothing));
      case uint16_t(Op::F64ConvertI32S):
      case uint16_t(Op::F64ConvertI32U):
        CHECK(iter.readConversion(ValType::I32, ValType::F64, &nothing));
      case uint16_t(Op::F64ConvertI64S):
      case uint16_t(Op::F64ConvertI64U):
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
      case uint16_t(Op::MemoryGrow): {
        uint32_t memoryIndex;
        CHECK(iter.readMemoryGrow(&memoryIndex, &nothing));
      }
      case uint16_t(Op::MemorySize): {
        uint32_t memoryIndex;
        CHECK(iter.readMemorySize(&memoryIndex));
      }
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
          case uint32_t(GcOp::StructNew): {
            uint32_t unusedUint;
            NothingVector unusedArgs{};
            CHECK(iter.readStructNew(&unusedUint, &unusedArgs));
          }
          case uint32_t(GcOp::StructNewDefault): {
            uint32_t unusedUint;
            CHECK(iter.readStructNewDefault(&unusedUint));
          }
          case uint32_t(GcOp::StructGet): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldWideningOp::None, &nothing));
          }
          case uint32_t(GcOp::StructGetS): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldWideningOp::Signed, &nothing));
          }
          case uint32_t(GcOp::StructGetU): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructGet(&unusedUint1, &unusedUint2,
                                     FieldWideningOp::Unsigned, &nothing));
          }
          case uint32_t(GcOp::StructSet): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readStructSet(&unusedUint1, &unusedUint2, &nothing,
                                     &nothing));
          }
          case uint32_t(GcOp::ArrayNew): {
            uint32_t unusedUint;
            CHECK(iter.readArrayNew(&unusedUint, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayNewFixed): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(
                iter.readArrayNewFixed(&unusedUint1, &unusedUint2, &nothings));
          }
          case uint32_t(GcOp::ArrayNewDefault): {
            uint32_t unusedUint;
            CHECK(iter.readArrayNewDefault(&unusedUint, &nothing));
          }
          case uint32_t(GcOp::ArrayNewData): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readArrayNewData(&unusedUint1, &unusedUint2, &nothing,
                                        &nothing));
          }
          case uint32_t(GcOp::ArrayNewElem): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readArrayNewElem(&unusedUint1, &unusedUint2, &nothing,
                                        &nothing));
          }
          case uint32_t(GcOp::ArrayInitData): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readArrayInitData(&unusedUint1, &unusedUint2, &nothing,
                                         &nothing, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayInitElem): {
            uint32_t unusedUint1, unusedUint2;
            CHECK(iter.readArrayInitElem(&unusedUint1, &unusedUint2, &nothing,
                                         &nothing, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayGet): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldWideningOp::None,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayGetS): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldWideningOp::Signed,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayGetU): {
            uint32_t unusedUint1;
            CHECK(iter.readArrayGet(&unusedUint1, FieldWideningOp::Unsigned,
                                    &nothing, &nothing));
          }
          case uint32_t(GcOp::ArraySet): {
            uint32_t unusedUint1;
            CHECK(
                iter.readArraySet(&unusedUint1, &nothing, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayLen): {
            CHECK(iter.readArrayLen(&nothing));
          }
          case uint32_t(GcOp::ArrayCopy): {
            int32_t unusedInt;
            bool unusedBool;
            CHECK(iter.readArrayCopy(&unusedInt, &unusedBool, &nothing,
                                     &nothing, &nothing, &nothing, &nothing));
          }
          case uint32_t(GcOp::ArrayFill): {
            uint32_t unusedTypeIndex;
            CHECK(iter.readArrayFill(&unusedTypeIndex, &nothing, &nothing,
                                     &nothing, &nothing));
          }
          case uint32_t(GcOp::RefI31): {
            CHECK(iter.readConversion(ValType::I32,
                                      ValType(RefType::i31().asNonNullable()),
                                      &nothing));
          }
          case uint32_t(GcOp::I31GetS): {
            CHECK(iter.readConversion(ValType(RefType::i31()), ValType::I32,
                                      &nothing));
          }
          case uint32_t(GcOp::I31GetU): {
            CHECK(iter.readConversion(ValType(RefType::i31()), ValType::I32,
                                      &nothing));
          }
          case uint16_t(GcOp::RefTest): {
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readRefTest(false, &unusedSourceType, &unusedDestType,
                                   &nothing));
          }
          case uint16_t(GcOp::RefTestNull): {
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readRefTest(true, &unusedSourceType, &unusedDestType,
                                   &nothing));
          }
          case uint16_t(GcOp::RefCast): {
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readRefCast(false, &unusedSourceType, &unusedDestType,
                                   &nothing));
          }
          case uint16_t(GcOp::RefCastNull): {
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readRefCast(true, &unusedSourceType, &unusedDestType,
                                   &nothing));
          }
          case uint16_t(GcOp::BrOnCast): {
            uint32_t unusedRelativeDepth;
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readBrOnCast(true, &unusedRelativeDepth,
                                    &unusedSourceType, &unusedDestType,
                                    &unusedType, &nothings));
          }
          case uint16_t(GcOp::BrOnCastFail): {
            uint32_t unusedRelativeDepth;
            RefType unusedSourceType;
            RefType unusedDestType;
            CHECK(iter.readBrOnCast(false, &unusedRelativeDepth,
                                    &unusedSourceType, &unusedDestType,
                                    &unusedType, &nothings));
          }
          case uint16_t(GcOp::AnyConvertExtern): {
            CHECK(iter.readRefConversion(RefType::extern_(), RefType::any(),
                                         &nothing));
          }
          case uint16_t(GcOp::ExternConvertAny): {
            CHECK(iter.readRefConversion(RefType::any(), RefType::extern_(),
                                         &nothing));
          }
          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
#endif

#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        if (!env.simdAvailable()) {
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
          case uint32_t(SimdOp::I8x16AddSatS):
          case uint32_t(SimdOp::I8x16AddSatU):
          case uint32_t(SimdOp::I8x16Sub):
          case uint32_t(SimdOp::I8x16SubSatS):
          case uint32_t(SimdOp::I8x16SubSatU):
          case uint32_t(SimdOp::I8x16MinS):
          case uint32_t(SimdOp::I8x16MinU):
          case uint32_t(SimdOp::I8x16MaxS):
          case uint32_t(SimdOp::I8x16MaxU):
          case uint32_t(SimdOp::I16x8Add):
          case uint32_t(SimdOp::I16x8AddSatS):
          case uint32_t(SimdOp::I16x8AddSatU):
          case uint32_t(SimdOp::I16x8Sub):
          case uint32_t(SimdOp::I16x8SubSatS):
          case uint32_t(SimdOp::I16x8SubSatU):
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
          case uint32_t(SimdOp::I8x16NarrowI16x8S):
          case uint32_t(SimdOp::I8x16NarrowI16x8U):
          case uint32_t(SimdOp::I16x8NarrowI32x4S):
          case uint32_t(SimdOp::I16x8NarrowI32x4U):
          case uint32_t(SimdOp::I8x16Swizzle):
          case uint32_t(SimdOp::F32x4PMax):
          case uint32_t(SimdOp::F32x4PMin):
          case uint32_t(SimdOp::F64x2PMax):
          case uint32_t(SimdOp::F64x2PMin):
          case uint32_t(SimdOp::I32x4DotI16x8S):
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16S):
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16S):
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16U):
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16U):
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8S):
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8S):
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8U):
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8U):
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4S):
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4S):
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4U):
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4U):
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK(iter.readBinary(ValType::V128, &nothing, &nothing));

          case uint32_t(SimdOp::I8x16Neg):
          case uint32_t(SimdOp::I16x8Neg):
          case uint32_t(SimdOp::I16x8ExtendLowI8x16S):
          case uint32_t(SimdOp::I16x8ExtendHighI8x16S):
          case uint32_t(SimdOp::I16x8ExtendLowI8x16U):
          case uint32_t(SimdOp::I16x8ExtendHighI8x16U):
          case uint32_t(SimdOp::I32x4Neg):
          case uint32_t(SimdOp::I32x4ExtendLowI16x8S):
          case uint32_t(SimdOp::I32x4ExtendHighI16x8S):
          case uint32_t(SimdOp::I32x4ExtendLowI16x8U):
          case uint32_t(SimdOp::I32x4ExtendHighI16x8U):
          case uint32_t(SimdOp::I32x4TruncSatF32x4S):
          case uint32_t(SimdOp::I32x4TruncSatF32x4U):
          case uint32_t(SimdOp::I64x2Neg):
          case uint32_t(SimdOp::I64x2ExtendLowI32x4S):
          case uint32_t(SimdOp::I64x2ExtendHighI32x4S):
          case uint32_t(SimdOp::I64x2ExtendLowI32x4U):
          case uint32_t(SimdOp::I64x2ExtendHighI32x4U):
          case uint32_t(SimdOp::F32x4Abs):
          case uint32_t(SimdOp::F32x4Neg):
          case uint32_t(SimdOp::F32x4Sqrt):
          case uint32_t(SimdOp::F32x4ConvertI32x4S):
          case uint32_t(SimdOp::F32x4ConvertI32x4U):
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
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16S):
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16U):
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8S):
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8U):
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
            CHECK(
                iter.readTernary(ValType::V128, &nothing, &nothing, &nothing));

          case uint32_t(SimdOp::I8x16Shuffle): {
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

          case uint32_t(SimdOp::V128Load8Splat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(1, &addr));
          }

          case uint32_t(SimdOp::V128Load16Splat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(2, &addr));
          }

          case uint32_t(SimdOp::V128Load32Splat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(4, &addr));
          }

          case uint32_t(SimdOp::V128Load64Splat): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadSplat(8, &addr));
          }

          case uint32_t(SimdOp::V128Load8x8S):
          case uint32_t(SimdOp::V128Load8x8U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadExtend(&addr));
          }

          case uint32_t(SimdOp::V128Load16x4S):
          case uint32_t(SimdOp::V128Load16x4U): {
            LinearMemoryAddress<Nothing> addr;
            CHECK(iter.readLoadExtend(&addr));
          }

          case uint32_t(SimdOp::V128Load32x2S):
          case uint32_t(SimdOp::V128Load32x2U): {
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

#  ifdef ENABLE_WASM_RELAXED_SIMD
          case uint32_t(SimdOp::F32x4RelaxedMadd):
          case uint32_t(SimdOp::F32x4RelaxedNmadd):
          case uint32_t(SimdOp::F64x2RelaxedMadd):
          case uint32_t(SimdOp::F64x2RelaxedNmadd):
          case uint32_t(SimdOp::I8x16RelaxedLaneSelect):
          case uint32_t(SimdOp::I16x8RelaxedLaneSelect):
          case uint32_t(SimdOp::I32x4RelaxedLaneSelect):
          case uint32_t(SimdOp::I64x2RelaxedLaneSelect):
          case uint32_t(SimdOp::I32x4DotI8x16I7x16AddS): {
            if (!env.v128RelaxedEnabled()) {
              return iter.unrecognizedOpcode(&op);
            }
            CHECK(
                iter.readTernary(ValType::V128, &nothing, &nothing, &nothing));
          }
          case uint32_t(SimdOp::F32x4RelaxedMin):
          case uint32_t(SimdOp::F32x4RelaxedMax):
          case uint32_t(SimdOp::F64x2RelaxedMin):
          case uint32_t(SimdOp::F64x2RelaxedMax):
          case uint32_t(SimdOp::I16x8RelaxedQ15MulrS):
          case uint32_t(SimdOp::I16x8DotI8x16I7x16S): {
            if (!env.v128RelaxedEnabled()) {
              return iter.unrecognizedOpcode(&op);
            }
            CHECK(iter.readBinary(ValType::V128, &nothing, &nothing));
          }
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4S):
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4U):
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2SZero):
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2UZero): {
            if (!env.v128RelaxedEnabled()) {
              return iter.unrecognizedOpcode(&op);
            }
            CHECK(iter.readUnary(ValType::V128, &nothing));
          }
          case uint32_t(SimdOp::I8x16RelaxedSwizzle): {
            if (!env.v128RelaxedEnabled()) {
              return iter.unrecognizedOpcode(&op);
            }
            CHECK(iter.readBinary(ValType::V128, &nothing, &nothing));
          }
#  endif

          default:
            return iter.unrecognizedOpcode(&op);
        }
        break;
      }
#endif  // ENABLE_WASM_SIMD

      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSatF32S):
          case uint32_t(MiscOp::I32TruncSatF32U):
            CHECK(iter.readConversion(ValType::F32, ValType::I32, &nothing));
          case uint32_t(MiscOp::I32TruncSatF64S):
          case uint32_t(MiscOp::I32TruncSatF64U):
            CHECK(iter.readConversion(ValType::F64, ValType::I32, &nothing));
          case uint32_t(MiscOp::I64TruncSatF32S):
          case uint32_t(MiscOp::I64TruncSatF32U):
            CHECK(iter.readConversion(ValType::F32, ValType::I64, &nothing));
          case uint32_t(MiscOp::I64TruncSatF64S):
          case uint32_t(MiscOp::I64TruncSatF64U):
            CHECK(iter.readConversion(ValType::F64, ValType::I64, &nothing));
          case uint32_t(MiscOp::MemoryCopy): {
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
          case uint32_t(MiscOp::MemoryFill): {
            uint32_t memoryIndex;
            CHECK(iter.readMemFill(&memoryIndex, &nothing, &nothing, &nothing));
          }
          case uint32_t(MiscOp::MemoryInit): {
            uint32_t unusedSegIndex;
            uint32_t unusedMemoryIndex;
            CHECK(iter.readMemOrTableInit(/*isMem=*/true, &unusedSegIndex,
                                          &unusedMemoryIndex, &nothing,
                                          &nothing, &nothing));
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
#ifdef ENABLE_WASM_MEMORY_CONTROL
          case uint32_t(MiscOp::MemoryDiscard): {
            if (!env.memoryControlEnabled()) {
              return iter.unrecognizedOpcode(&op);
            }
            uint32_t unusedMemoryIndex;
            CHECK(iter.readMemDiscard(&unusedMemoryIndex, &nothing, &nothing));
          }
#endif
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
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::RefAsNonNull): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        CHECK(iter.readRefAsNonNull(&nothing));
      }
      case uint16_t(Op::BrOnNull): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedDepth;
        CHECK(
            iter.readBrOnNull(&unusedDepth, &unusedType, &nothings, &nothing));
      }
      case uint16_t(Op::BrOnNonNull): {
        if (!env.gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        uint32_t unusedDepth;
        CHECK(iter.readBrOnNonNull(&unusedDepth, &unusedType, &nothings,
                                   &nothing));
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
      case uint16_t(Op::Try):
        CHECK(iter.readTry(&unusedType));
      case uint16_t(Op::Catch): {
        LabelKind unusedKind;
        uint32_t unusedIndex;
        CHECK(iter.readCatch(&unusedKind, &unusedIndex, &unusedType,
                             &unusedType, &nothings));
      }
      case uint16_t(Op::CatchAll): {
        LabelKind unusedKind;
        CHECK(iter.readCatchAll(&unusedKind, &unusedType, &unusedType,
                                &nothings));
      }
      case uint16_t(Op::Delegate): {
        uint32_t unusedDepth;
        if (!iter.readDelegate(&unusedDepth, &unusedType, &nothings)) {
          return false;
        }
        iter.popDelegate();
        break;
      }
      case uint16_t(Op::Throw): {
        uint32_t unusedIndex;
        CHECK(iter.readThrow(&unusedIndex, &nothings));
      }
      case uint16_t(Op::Rethrow): {
        uint32_t unusedDepth;
        CHECK(iter.readRethrow(&unusedDepth));
      }
      case uint16_t(Op::ThrowRef): {
        if (!env.exnrefEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        CHECK(iter.readThrowRef(&nothing));
      }
      case uint16_t(Op::TryTable): {
        if (!env.exnrefEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        TryTableCatchVector catches;
        CHECK(iter.readTryTable(&unusedType, &catches));
      }
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
  const uint8_t* bodyBegin = d.currentPosition();

  ValTypeVector locals;
  if (!DecodeLocalEntriesWithParams(d, env, funcIndex, &locals)) {
    return false;
  }

  return DecodeFunctionBodyExprs(env, funcIndex, locals, bodyBegin + bodySize,
                                 &d);
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

static bool DecodeValTypeVector(Decoder& d, ModuleEnvironment* env,
                                uint32_t count, ValTypeVector* valTypes) {
  if (!valTypes->resize(count)) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!d.readValType(*env->types, env->features, &(*valTypes)[i])) {
      return false;
    }
  }
  return true;
}

static bool DecodeFuncType(Decoder& d, ModuleEnvironment* env,
                           FuncType* funcType) {
  uint32_t numArgs;
  if (!d.readVarU32(&numArgs)) {
    return d.fail("bad number of function args");
  }
  if (numArgs > MaxParams) {
    return d.fail("too many arguments in signature");
  }
  ValTypeVector args;
  if (!DecodeValTypeVector(d, env, numArgs, &args)) {
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
  if (!DecodeValTypeVector(d, env, numResults, &results)) {
    return false;
  }

  *funcType = FuncType(std::move(args), std::move(results));
  return true;
}

static bool DecodeStructType(Decoder& d, ModuleEnvironment* env,
                             StructType* structType) {
  if (!env->gcEnabled()) {
    return d.fail("gc not enabled");
  }

  uint32_t numFields;
  if (!d.readVarU32(&numFields)) {
    return d.fail("Bad number of fields");
  }

  if (numFields > MaxStructFields) {
    return d.fail("too many fields in struct");
  }

  FieldTypeVector fields;
  if (!fields.resize(numFields)) {
    return false;
  }

  for (uint32_t i = 0; i < numFields; i++) {
    if (!d.readStorageType(*env->types, env->features, &fields[i].type)) {
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
  }

  *structType = StructType(std::move(fields));

  // Compute the struct layout, and fail if the struct is too large
  if (!structType->init()) {
    return d.fail("too many fields in struct");
  }
  return true;
}

static bool DecodeArrayType(Decoder& d, ModuleEnvironment* env,
                            ArrayType* arrayType) {
  if (!env->gcEnabled()) {
    return d.fail("gc not enabled");
  }

  StorageType elementType;
  if (!d.readStorageType(*env->types, env->features, &elementType)) {
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

  *arrayType = ArrayType(elementType, isMutable);
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

  uint32_t numRecGroups;
  if (!d.readVarU32(&numRecGroups)) {
    return d.fail("expected number of types");
  }

  // Check if we've reached our implementation defined limit of recursion
  // groups.
  if (numRecGroups > MaxRecGroups) {
    return d.fail("too many types");
  }

  for (uint32_t recGroupIndex = 0; recGroupIndex < numRecGroups;
       recGroupIndex++) {
    uint32_t recGroupLength = 1;

    // Decode an optional recursion group length, if the GC proposal is
    // enabled.
    if (env->gcEnabled()) {
      uint8_t firstTypeCode;
      if (!d.peekByte(&firstTypeCode)) {
        return d.fail("expected type form");
      }

      if (firstTypeCode == (uint8_t)TypeCode::RecGroup) {
        // Skip over the prefix byte that was peeked.
        d.uncheckedReadFixedU8();

        // Read the number of types in this recursion group
        if (!d.readVarU32(&recGroupLength)) {
          return d.fail("expected recursion group length");
        }
      }
    }

    // Start a recursion group. This will extend the type context with empty
    // type definitions to be filled.
    MutableRecGroup recGroup = env->types->startRecGroup(recGroupLength);
    if (!recGroup) {
      return false;
    }

    // First, iterate over the types, validate them and set super types.
    // Subtyping relationship will be checked in a second iteration.
    for (uint32_t recGroupTypeIndex = 0; recGroupTypeIndex < recGroupLength;
         recGroupTypeIndex++) {
      uint32_t typeIndex =
          env->types->length() - recGroupLength + recGroupTypeIndex;

      // Check if we've reached our implementation defined limit of type
      // definitions.
      if (typeIndex >= MaxTypes) {
        return d.fail("too many types");
      }

      uint8_t form;
      const TypeDef* superTypeDef = nullptr;

      // By default, all types are final unless the sub keyword is specified.
      bool finalTypeFlag = true;

      // Decode an optional declared super type index, if the GC proposal is
      // enabled.
      if (env->gcEnabled() && d.peekByte(&form) &&
          (form == (uint8_t)TypeCode::SubNoFinalType ||
           form == (uint8_t)TypeCode::SubFinalType)) {
        if (form == (uint8_t)TypeCode::SubNoFinalType) {
          finalTypeFlag = false;
        }

        // Skip over the `sub` or `final` prefix byte we peeked.
        d.uncheckedReadFixedU8();

        // Decode the number of super types, which is currently limited to at
        // most one.
        uint32_t numSuperTypes;
        if (!d.readVarU32(&numSuperTypes)) {
          return d.fail("expected number of super types");
        }
        if (numSuperTypes > 1) {
          return d.fail("too many super types");
        }

        // Decode the super type, if any.
        if (numSuperTypes == 1) {
          uint32_t superTypeDefIndex;
          if (!d.readVarU32(&superTypeDefIndex)) {
            return d.fail("expected super type index");
          }

          // A super type index must be strictly less than the current type
          // index in order to avoid cycles.
          if (superTypeDefIndex >= typeIndex) {
            return d.fail("invalid super type index");
          }

          superTypeDef = &env->types->type(superTypeDefIndex);
        }
      }

      // Decode the kind of type definition
      if (!d.readFixedU8(&form)) {
        return d.fail("expected type form");
      }

      TypeDef* typeDef = &recGroup->type(recGroupTypeIndex);
      switch (form) {
        case uint8_t(TypeCode::Func): {
          FuncType funcType;
          if (!DecodeFuncType(d, env, &funcType)) {
            return false;
          }
          *typeDef = std::move(funcType);
          break;
        }
        case uint8_t(TypeCode::Struct): {
          StructType structType;
          if (!DecodeStructType(d, env, &structType)) {
            return false;
          }
          *typeDef = std::move(structType);
          break;
        }
        case uint8_t(TypeCode::Array): {
          ArrayType arrayType;
          if (!DecodeArrayType(d, env, &arrayType)) {
            return false;
          }
          *typeDef = std::move(arrayType);
          break;
        }
        default:
          return d.fail("expected type form");
      }

      typeDef->setFinal(finalTypeFlag);
      if (superTypeDef) {
        // Check that we aren't creating too deep of a subtyping chain
        if (superTypeDef->subTypingDepth() >= MaxSubTypingDepth) {
          return d.fail("type is too deep");
        }

        typeDef->setSuperTypeDef(superTypeDef);
      }

      if (typeDef->isFuncType()) {
        typeDef->funcType().initImmediateTypeId(
            env->gcEnabled(), typeDef->isFinal(), superTypeDef, recGroupLength);
      }
    }

    // Check the super types to make sure they are compatible with their
    // subtypes. This is done in a second iteration to avoid dealing with not
    // yet loaded types.
    for (uint32_t recGroupTypeIndex = 0; recGroupTypeIndex < recGroupLength;
         recGroupTypeIndex++) {
      TypeDef* typeDef = &recGroup->type(recGroupTypeIndex);
      if (typeDef->superTypeDef()) {
        // Check that the super type is compatible with this type
        if (!TypeDef::canBeSubTypeOf(typeDef, typeDef->superTypeDef())) {
          return d.fail("incompatible super type");
        }
      }
    }

    // Finish the recursion group, which will canonicalize the types.
    if (!env->types->endRecGroup()) {
      return false;
    }
  }

  return d.finishSection(*range, "type");
}

[[nodiscard]] static bool DecodeName(Decoder& d, CacheableName* name) {
  uint32_t numBytes;
  if (!d.readVarU32(&numBytes)) {
    return false;
  }

  if (numBytes > MaxStringBytes) {
    return false;
  }

  const uint8_t* bytes;
  if (!d.readBytes(numBytes, &bytes)) {
    return false;
  }

  if (!IsUtf8(AsChars(Span(bytes, numBytes)))) {
    return false;
  }

  UTF8Bytes utf8Bytes;
  if (!utf8Bytes.resizeUninitialized(numBytes)) {
    return false;
  }
  memcpy(utf8Bytes.begin(), bytes, numBytes);

  *name = CacheableName(std::move(utf8Bytes));
  return true;
}

static bool DecodeFuncTypeIndex(Decoder& d, const SharedTypeContext& types,
                                uint32_t* funcTypeIndex) {
  if (!d.readVarU32(funcTypeIndex)) {
    return d.fail("expected signature index");
  }

  if (*funcTypeIndex >= types->length()) {
    return d.fail("signature index out of range");
  }

  const TypeDef& def = (*types)[*funcTypeIndex];

  if (!def.isFuncType()) {
    return d.fail("signature index references non-signature");
  }

  return true;
}

static bool DecodeLimitBound(Decoder& d, IndexType indexType, uint64_t* bound) {
  if (indexType == IndexType::I64) {
    return d.readVarU64(bound);
  }

  // Spec tests assert that we only decode a LEB32 when index type is I32.
  uint32_t bound32;
  if (!d.readVarU32(&bound32)) {
    return false;
  }
  *bound = bound32;
  return true;
}

static bool DecodeLimits(Decoder& d, LimitsKind kind, Limits* limits) {
  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected flags");
  }

  uint8_t mask = kind == LimitsKind::Memory ? uint8_t(LimitsMask::Memory)
                                            : uint8_t(LimitsMask::Table);

  if (flags & ~uint8_t(mask)) {
    return d.failf("unexpected bits set in flags: %" PRIu32,
                   uint32_t(flags & ~uint8_t(mask)));
  }

  // Memory limits may be shared or specify an alternate index type
  if (kind == LimitsKind::Memory) {
    if ((flags & uint8_t(LimitsFlags::IsShared)) &&
        !(flags & uint8_t(LimitsFlags::HasMaximum))) {
      return d.fail("maximum length required for shared memory");
    }

    limits->shared = (flags & uint8_t(LimitsFlags::IsShared))
                         ? Shareable::True
                         : Shareable::False;

#ifdef ENABLE_WASM_MEMORY64
    limits->indexType =
        (flags & uint8_t(LimitsFlags::IsI64)) ? IndexType::I64 : IndexType::I32;
#else
    limits->indexType = IndexType::I32;
    if (flags & uint8_t(LimitsFlags::IsI64)) {
      return d.fail("i64 is not supported for memory limits");
    }
#endif
  } else {
    limits->shared = Shareable::False;
    limits->indexType = IndexType::I32;
  }

  uint64_t initial;
  if (!DecodeLimitBound(d, limits->indexType, &initial)) {
    return d.fail("expected initial length");
  }
  limits->initial = initial;

  if (flags & uint8_t(LimitsFlags::HasMaximum)) {
    uint64_t maximum;
    if (!DecodeLimitBound(d, limits->indexType, &maximum)) {
      return d.fail("expected maximum length");
    }

    if (limits->initial > maximum) {
      return d.failf(
          "memory size minimum must not be greater than maximum; "
          "maximum length %" PRIu64 " is less than initial length %" PRIu64,
          maximum, limits->initial);
    }

    limits->maximum.emplace(maximum);
  }

  return true;
}

static bool DecodeTableTypeAndLimits(Decoder& d, ModuleEnvironment* env) {
  bool initExprPresent = false;
  uint8_t typeCode;
  if (!d.peekByte(&typeCode)) {
    return d.fail("expected type code");
  }
  if (typeCode == (uint8_t)TypeCode::TableHasInitExpr) {
    d.uncheckedReadFixedU8();
    uint8_t flags;
    if (!d.readFixedU8(&flags) || flags != 0) {
      return d.fail("expected reserved byte to be 0");
    }
    initExprPresent = true;
  }

  RefType tableElemType;
  if (!d.readRefType(*env->types, env->features, &tableElemType)) {
    return false;
  }

  Limits limits;
  if (!DecodeLimits(d, LimitsKind::Table, &limits)) {
    return false;
  }

  // Decoding limits for a table only supports i32
  MOZ_ASSERT(limits.indexType == IndexType::I32);

  // If there's a maximum, check it is in range.  The check to exclude
  // initial > maximum is carried out by the DecodeLimits call above, so
  // we don't repeat it here.
  if (limits.initial > MaxTableLimitField ||
      ((limits.maximum.isSome() &&
        limits.maximum.value() > MaxTableLimitField))) {
    return d.fail("too many table elements");
  }

  if (env->tables.length() >= MaxTables) {
    return d.fail("too many tables");
  }

  // The rest of the runtime expects table limits to be within a 32-bit range.
  static_assert(MaxTableLimitField <= UINT32_MAX, "invariant");
  uint32_t initialLength = uint32_t(limits.initial);
  Maybe<uint32_t> maximumLength;
  if (limits.maximum) {
    maximumLength = Some(uint32_t(*limits.maximum));
  }

  Maybe<InitExpr> initExpr;
  if (initExprPresent) {
    InitExpr initializer;
    if (!InitExpr::decodeAndValidate(d, env, tableElemType, &initializer)) {
      return false;
    }
    initExpr = Some(std::move(initializer));
  } else {
    if (!tableElemType.isNullable()) {
      return d.fail("table with non-nullable references requires initializer");
    }
  }

  return env->tables.emplaceBack(tableElemType, initialLength, maximumLength,
                                 std::move(initExpr), /* isAsmJS */ false);
}

static bool DecodeGlobalType(Decoder& d, const SharedTypeContext& types,
                             const FeatureArgs& features, ValType* type,
                             bool* isMutable) {
  if (!d.readValType(*types, features, type)) {
    return d.fail("expected global type");
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

static bool DecodeMemoryTypeAndLimits(Decoder& d, ModuleEnvironment* env,
                                      MemoryDescVector* memories) {
  if (!env->features.multiMemory && env->numMemories() == 1) {
    return d.fail("already have default memory");
  }

  if (env->numMemories() >= MaxMemories) {
    return d.fail("too many memories");
  }

  Limits limits;
  if (!DecodeLimits(d, LimitsKind::Memory, &limits)) {
    return false;
  }

  uint64_t maxField = MaxMemoryLimitField(limits.indexType);

  if (limits.initial > maxField) {
    return d.fail("initial memory size too big");
  }

  if (limits.maximum && *limits.maximum > maxField) {
    return d.fail("maximum memory size too big");
  }

  if (limits.shared == Shareable::True &&
      env->sharedMemoryEnabled() == Shareable::False) {
    return d.fail("shared memory is disabled");
  }

  if (limits.indexType == IndexType::I64 && !env->memory64Enabled()) {
    return d.fail("memory64 is disabled");
  }

  return memories->emplaceBack(MemoryDesc(limits));
}

static bool DecodeTag(Decoder& d, ModuleEnvironment* env, TagKind* tagKind,
                      uint32_t* funcTypeIndex) {
  uint32_t tagCode;
  if (!d.readVarU32(&tagCode)) {
    return d.fail("expected tag kind");
  }

  if (TagKind(tagCode) != TagKind::Exception) {
    return d.fail("illegal tag kind");
  }
  *tagKind = TagKind(tagCode);

  if (!d.readVarU32(funcTypeIndex)) {
    return d.fail("expected function index in tag");
  }
  if (*funcTypeIndex >= env->numTypes()) {
    return d.fail("function type index in tag out of bounds");
  }
  if (!(*env->types)[*funcTypeIndex].isFuncType()) {
    return d.fail("function type index must index a function type");
  }
  if ((*env->types)[*funcTypeIndex].funcType().results().length() != 0) {
    return d.fail("tag function types must not return anything");
  }
  return true;
}

static bool DecodeImport(Decoder& d, ModuleEnvironment* env) {
  CacheableName moduleName;
  if (!DecodeName(d, &moduleName)) {
    return d.fail("expected valid import module name");
  }

  CacheableName fieldName;
  if (!DecodeName(d, &fieldName)) {
    return d.fail("expected valid import field name");
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
      if (!env->funcs.append(FuncDesc(
              &env->types->type(funcTypeIndex).funcType(), funcTypeIndex))) {
        return false;
      }
      if (env->funcs.length() > MaxFuncs) {
        return d.fail("too many functions");
      }
      break;
    }
    case DefinitionKind::Table: {
      if (!DecodeTableTypeAndLimits(d, env)) {
        return false;
      }
      env->tables.back().isImported = true;
      break;
    }
    case DefinitionKind::Memory: {
      if (!DecodeMemoryTypeAndLimits(d, env, &env->memories)) {
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
      if (!env->globals.append(
              GlobalDesc(type, isMutable, env->globals.length()))) {
        return false;
      }
      if (env->globals.length() > MaxGlobals) {
        return d.fail("too many globals");
      }
      break;
    }
    case DefinitionKind::Tag: {
      TagKind tagKind;
      uint32_t funcTypeIndex;
      if (!DecodeTag(d, env, &tagKind, &funcTypeIndex)) {
        return false;
      }
      ValTypeVector args;
      if (!args.appendAll((*env->types)[funcTypeIndex].funcType().args())) {
        return false;
      }
      MutableTagType tagType = js_new<TagType>();
      if (!tagType || !tagType->initialize(std::move(args))) {
        return false;
      }
      if (!env->tags.emplaceBack(tagKind, tagType)) {
        return false;
      }
      if (env->tags.length() > MaxTags) {
        return d.fail("too many tags");
      }
      break;
    }
    default:
      return d.fail("unsupported import kind");
  }

  return env->imports.emplaceBack(std::move(moduleName), std::move(fieldName),
                                  importKind);
}

static bool CheckImportsAgainstBuiltinModules(Decoder& d,
                                              ModuleEnvironment* env) {
  const BuiltinModuleIds& builtinModules = env->features.builtinModules;

  // Skip this pass if there are no builtin modules enabled
  if (builtinModules.hasNone()) {
    return true;
  }

  uint32_t importFuncIndex = 0;
  for (auto& import : env->imports) {
    Maybe<BuiltinModuleId> builtinModule =
        ImportMatchesBuiltinModule(import.module.utf8Bytes(), builtinModules);

    switch (import.kind) {
      case DefinitionKind::Function: {
        const FuncDesc& func = env->funcs[importFuncIndex];
        importFuncIndex += 1;

        // Skip this import if it doesn't refer to a builtin module. We do have
        // to increment the import function index regardless though.
        if (!builtinModule) {
          continue;
        }

        // Check if this import refers to a builtin module function
        Maybe<const BuiltinModuleFunc*> builtinFunc =
            ImportMatchesBuiltinModuleFunc(import.field.utf8Bytes(),
                                           *builtinModule);
        if (!builtinFunc) {
          return d.fail("unrecognized builtin module field");
        }

        const TypeDef& importTypeDef = (*env->types)[func.typeIndex];
        if (!TypeDef::isSubTypeOf((*builtinFunc)->typeDef(), &importTypeDef)) {
          return d.failf("type mismatch in %s", (*builtinFunc)->exportName());
        }
        break;
      }
      default: {
        if (!builtinModule) {
          continue;
        }
        return d.fail("unrecognized builtin import");
      }
    }
  }

  return true;
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

  for (uint32_t i = 0; i < numImports; i++) {
    if (!DecodeImport(d, env)) {
      return false;
    }
  }

  if (!d.finishSection(*range, "import")) {
    return false;
  }

  env->numFuncImports = env->funcs.length();
  env->numGlobalImports = env->globals.length();
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
    env->funcs.infallibleAppend(
        FuncDesc(&env->types->type(funcTypeIndex).funcType(), funcTypeIndex));
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
    if (!DecodeTableTypeAndLimits(d, env)) {
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

  if (!env->features.multiMemory && numMemories > 1) {
    return d.fail("the number of memories must be at most one");
  }

  for (uint32_t i = 0; i < numMemories; ++i) {
    if (!DecodeMemoryTypeAndLimits(d, env, &env->memories)) {
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

static bool DecodeTagSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startSection(SectionId::Tag, env, &range, "tag")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of tags");
  }

  CheckedInt<uint32_t> numTags = env->tags.length();
  numTags += numDefs;
  if (!numTags.isValid() || numTags.value() > MaxTags) {
    return d.fail("too many tags");
  }

  if (!env->tags.reserve(numTags.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    TagKind tagKind;
    uint32_t funcTypeIndex;
    if (!DecodeTag(d, env, &tagKind, &funcTypeIndex)) {
      return false;
    }
    ValTypeVector args;
    if (!args.appendAll((*env->types)[funcTypeIndex].funcType().args())) {
      return false;
    }
    MutableTagType tagType = js_new<TagType>();
    if (!tagType || !tagType->initialize(std::move(args))) {
      return false;
    }
    env->tags.infallibleEmplaceBack(tagKind, tagType);
  }

  return d.finishSection(*range, "tag");
}

using NameSet = HashSet<Span<char>, NameHasher, SystemAllocPolicy>;

[[nodiscard]] static bool DecodeExportName(Decoder& d, NameSet* dupSet,
                                           CacheableName* exportName) {
  if (!DecodeName(d, exportName)) {
    d.fail("expected valid export name");
    return false;
  }

  NameSet::AddPtr p = dupSet->lookupForAdd(exportName->utf8Bytes());
  if (p) {
    d.fail("duplicate export");
    return false;
  }

  return dupSet->add(p, exportName->utf8Bytes());
}

static bool DecodeExport(Decoder& d, ModuleEnvironment* env, NameSet* dupSet) {
  CacheableName fieldName;
  if (!DecodeExportName(d, dupSet, &fieldName)) {
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
      env->tables[tableIndex].isExported = true;
      return env->exports.emplaceBack(std::move(fieldName), tableIndex,
                                      DefinitionKind::Table);
    }
    case DefinitionKind::Memory: {
      uint32_t memoryIndex;
      if (!d.readVarU32(&memoryIndex)) {
        return d.fail("expected memory index");
      }

      if (memoryIndex >= env->numMemories()) {
        return d.fail("exported memory index out of bounds");
      }

      return env->exports.emplaceBack(std::move(fieldName), memoryIndex,
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

      return env->exports.emplaceBack(std::move(fieldName), globalIndex,
                                      DefinitionKind::Global);
    }
    case DefinitionKind::Tag: {
      uint32_t tagIndex;
      if (!d.readVarU32(&tagIndex)) {
        return d.fail("expected tag index");
      }
      if (tagIndex >= env->tags.length()) {
        return d.fail("exported tag index out of bounds");
      }

      env->tags[tagIndex].isExport = true;
      return env->exports.emplaceBack(std::move(fieldName), tagIndex,
                                      DefinitionKind::Tag);
    }
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

  NameSet dupSet;

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

static inline ModuleElemSegment::Kind NormalizeElemSegmentKind(
    ElemSegmentKind decodedKind) {
  switch (decodedKind) {
    case ElemSegmentKind::Active:
    case ElemSegmentKind::ActiveWithTableIndex: {
      return ModuleElemSegment::Kind::Active;
    }
    case ElemSegmentKind::Passive: {
      return ModuleElemSegment::Kind::Passive;
    }
    case ElemSegmentKind::Declared: {
      return ModuleElemSegment::Kind::Declared;
    }
  }
  MOZ_CRASH("unexpected elem segment kind");
}

static bool DecodeElemSegment(Decoder& d, ModuleEnvironment* env) {
  uint32_t segmentFlags;
  if (!d.readVarU32(&segmentFlags)) {
    return d.fail("expected elem segment flags field");
  }

  Maybe<ElemSegmentFlags> flags = ElemSegmentFlags::construct(segmentFlags);
  if (!flags) {
    return d.fail("invalid elem segment flags field");
  }

  ModuleElemSegment seg = ModuleElemSegment();

  ElemSegmentKind segmentKind = flags->kind();
  seg.kind = NormalizeElemSegmentKind(segmentKind);

  if (segmentKind == ElemSegmentKind::Active ||
      segmentKind == ElemSegmentKind::ActiveWithTableIndex) {
    if (env->tables.length() == 0) {
      return d.fail("active elem segment requires a table");
    }

    uint32_t tableIndex = 0;
    if (segmentKind == ElemSegmentKind::ActiveWithTableIndex &&
        !d.readVarU32(&tableIndex)) {
      return d.fail("expected table index");
    }
    if (tableIndex >= env->tables.length()) {
      return d.fail("table index out of range for element segment");
    }
    seg.tableIndex = tableIndex;

    InitExpr offset;
    if (!InitExpr::decodeAndValidate(d, env, ValType::I32, &offset)) {
      return false;
    }
    seg.offsetIfActive.emplace(std::move(offset));
  } else {
    // Too many bugs result from keeping this value zero.  For passive
    // or declared segments, there really is no table index, and we should
    // never touch the field.
    MOZ_ASSERT(segmentKind == ElemSegmentKind::Passive ||
               segmentKind == ElemSegmentKind::Declared);
    seg.tableIndex = (uint32_t)-1;
  }

  ElemSegmentPayload payload = flags->payload();
  RefType elemType;

  // `ActiveWithTableIndex`, `Declared`, and `Passive` element segments encode
  // the type or definition kind of the payload. `Active` element segments are
  // restricted to MVP behavior, which assumes only function indices.
  if (segmentKind == ElemSegmentKind::Active) {
    elemType = RefType::func();
  } else {
    switch (payload) {
      case ElemSegmentPayload::Expressions: {
        if (!d.readRefType(*env->types, env->features, &elemType)) {
          return false;
        }
      } break;
      case ElemSegmentPayload::Indices: {
        uint8_t elemKind;
        if (!d.readFixedU8(&elemKind)) {
          return d.fail("expected element kind");
        }

        if (elemKind != uint8_t(DefinitionKind::Function)) {
          return d.fail("invalid element kind");
        }
        elemType = RefType::func();
      } break;
    }
  }

  // For active segments, check if the element type is compatible with the
  // destination table type.
  if (seg.active()) {
    RefType tblElemType = env->tables[seg.tableIndex].elemType;
    if (!CheckIsSubtypeOf(d, *env, d.currentOffset(),
                          ValType(elemType).storageType(),
                          ValType(tblElemType).storageType())) {
      return false;
    }
  }
  seg.elemType = elemType;

  uint32_t numElems;
  if (!d.readVarU32(&numElems)) {
    return d.fail("expected element segment size");
  }

  if (numElems > MaxElemSegmentLength) {
    return d.fail("too many elements in element segment");
  }

  bool isAsmJS = seg.active() && env->tables[seg.tableIndex].isAsmJS;

  switch (payload) {
    case ElemSegmentPayload::Indices: {
      seg.encoding = ModuleElemSegment::Encoding::Indices;
      if (!seg.elemIndices.reserve(numElems)) {
        return false;
      }

      for (uint32_t i = 0; i < numElems; i++) {
        uint32_t elemIndex;
        if (!d.readVarU32(&elemIndex)) {
          return d.fail("failed to read element index");
        }
        // The only valid type of index right now is a function index.
        if (elemIndex >= env->numFuncs()) {
          return d.fail("element index out of range");
        }

        seg.elemIndices.infallibleAppend(elemIndex);
        if (!isAsmJS) {
          env->declareFuncExported(elemIndex, /*eager=*/false,
                                   /*canRefFunc=*/true);
        }
      }
    } break;
    case ElemSegmentPayload::Expressions: {
      seg.encoding = ModuleElemSegment::Encoding::Expressions;
      const uint8_t* exprsStart = d.currentPosition();
      seg.elemExpressions.count = numElems;
      for (uint32_t i = 0; i < numElems; i++) {
        Maybe<LitVal> unusedLiteral;
        if (!DecodeConstantExpression(d, env, elemType, &unusedLiteral)) {
          return false;
        }
      }
      const uint8_t* exprsEnd = d.currentPosition();
      if (!seg.elemExpressions.exprBytes.append(exprsStart, exprsEnd)) {
        return false;
      }
    } break;
  }

  env->elemSegments.infallibleAppend(std::move(seg));
  return true;
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
    if (!DecodeElemSegment(d, env)) {
      return false;
    }
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

#ifdef ENABLE_WASM_BRANCH_HINTING
static bool ParseBranchHintingSection(Decoder& d, ModuleEnvironment* env) {
  uint32_t functionCount;
  if (!d.readVarU32(&functionCount)) {
    return d.fail("failed to read function count");
  }

  for (uint32_t i = 0; i < functionCount; i++) {
    uint32_t functionIndex;
    if (!d.readVarU32(&functionIndex)) {
      return d.fail("failed to read function index");
    }

    // Disallow branch hints on imported functions.
    if ((functionIndex >= env->funcs.length()) ||
        (functionIndex < env->numFuncImports)) {
      return d.fail("invalid function index in branch hint");
    }

    uint32_t hintCount;
    if (!d.readVarU32(&hintCount)) {
      return d.fail("failed to read hint count");
    }

    BranchHintVector hintVector;
    if (!hintVector.reserve(hintCount)) {
      return false;
    }

    // Branch hint offsets must appear in increasing byte offset order, at most
    // once for each offset.
    uint32_t prevOffsetPlus1 = 0;
    for (uint32_t hintIndex = 0; hintIndex < hintCount; hintIndex++) {
      uint32_t branchOffset;
      if (!d.readVarU32(&branchOffset)) {
        return d.fail("failed to read branch offset");
      }
      if (branchOffset <= prevOffsetPlus1) {
        return d.fail("Invalid offset in code hint");
      }

      uint32_t reserved;
      if (!d.readVarU32(&reserved) || (reserved != 1)) {
        return d.fail("Invalid reserved value for code hint");
      }

      uint32_t branchHintValue;
      if (!d.readVarU32(&branchHintValue) ||
          (branchHintValue >= MaxBranchHintValue)) {
        return d.fail("Invalid branch hint value");
      }

      BranchHint branchHint = static_cast<BranchHint>(branchHintValue);
      BranchHintEntry entry(branchOffset, branchHint);
      hintVector.infallibleAppend(entry);

      prevOffsetPlus1 = branchOffset;
    }

    // Save this collection in the module
    if (!env->branchHints.addHintsForFunc(functionIndex,
                                          std::move(hintVector))) {
      return false;
    }
  }

  return true;
}

static bool DecodeBranchHintingSection(Decoder& d, ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!d.startCustomSection(BranchHintingSectionName, env, &range)) {
    return false;
  }
  if (!range) {
    return true;
  }

  // Skip this custom section if errors are encountered during parsing.
  env->parsedBranchHints = ParseBranchHintingSection(d, env);

  d.finishCustomSection(BranchHintingSectionName, *range);
  return true;
}
#endif

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

  // Eagerly check imports for future link errors against any known builtin
  // module.
  if (!CheckImportsAgainstBuiltinModules(d, env)) {
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

  if (!DecodeTagSection(d, env)) {
    return false;
  }

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

#ifdef ENABLE_WASM_BRANCH_HINTING
  if (env->branchHintingEnabled() && !DecodeBranchHintingSection(d, env)) {
    return false;
  }
#endif

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
    if (!DecodeFunctionBody(d, *env, env->numFuncImports + funcDefIndex)) {
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

    if (initializerKind != DataSegmentKind::Passive &&
        env->numMemories() == 0) {
      return d.fail("active data segment requires a memory section");
    }

    DataSegmentEnv seg;
    if (initializerKind == DataSegmentKind::ActiveWithMemoryIndex) {
      if (!d.readVarU32(&seg.memoryIndex)) {
        return d.fail("expected memory index");
      }
    } else if (initializerKind == DataSegmentKind::Active) {
      seg.memoryIndex = 0;
    } else {
      seg.memoryIndex = InvalidMemoryIndex;
    }

    if (initializerKind == DataSegmentKind::Active ||
        initializerKind == DataSegmentKind::ActiveWithMemoryIndex) {
      if (seg.memoryIndex >= env->numMemories()) {
        return d.fail("invalid memory index");
      }

      InitExpr segOffset;
      ValType exprType = ToValType(env->memories[seg.memoryIndex].indexType());
      if (!InitExpr::decodeAndValidate(d, env, exprType, &segOffset)) {
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
  if (!env.init()) {
    return false;
  }

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
