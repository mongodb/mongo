/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#include "wasm/WasmInitExpr.h"

#include "mozilla/Maybe.h"

#include "wasm/WasmInstance.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

static bool ValidateInitExpr(Decoder& d, ModuleEnvironment* env,
                             ValType expected, Maybe<LitVal>* literal) {
  ValidatingOpIter iter(*env, d, ValidatingOpIter::InitExpr);

  if (!iter.startInitExpr(expected)) {
    return false;
  }

  // Perform trivial constant recovery, this is done so that codegen may
  // generate optimal code for global.get on immutable globals with simple
  // initializers.
  //
  // We simply update the last seen literal value while validating an
  // instruction with a literal value, and clear the literal value when
  // validating an instruction with a dynamic value. The last value is the
  // literal for this init expressions, if any. This is correct because there
  // are no drops or control flow allowed in init expressions.
  *literal = Nothing();

  while (true) {
    OpBytes op;
    if (!iter.readOp(&op)) {
      return false;
    }

#ifdef ENABLE_WASM_EXTENDED_CONST
    Nothing nothing;
#endif
    NothingVector nothings{};
    ResultType unusedType;

    switch (op.b0) {
      case uint16_t(Op::End): {
        LabelKind kind;
        if (!iter.readEnd(&kind, &unusedType, &nothings, &nothings)) {
          return false;
        }
        MOZ_ASSERT(kind == LabelKind::Body);
        iter.popEnd();
        if (iter.controlStackEmpty()) {
          return iter.endInitExpr();
        }
        break;
      }
      case uint16_t(Op::GetGlobal): {
        uint32_t index;
        if (!iter.readGetGlobal(&index)) {
          return false;
        }
        *literal = Nothing();
        break;
      }
      case uint16_t(Op::I32Const): {
        int32_t c;
        if (!iter.readI32Const(&c)) {
          return false;
        }
        *literal = Some(LitVal(uint32_t(c)));
        break;
      }
      case uint16_t(Op::I64Const): {
        int64_t c;
        if (!iter.readI64Const(&c)) {
          return false;
        }
        *literal = Some(LitVal(uint64_t(c)));
        break;
      }
      case uint16_t(Op::F32Const): {
        float c;
        if (!iter.readF32Const(&c)) {
          return false;
        }
        *literal = Some(LitVal(c));
        break;
      }
      case uint16_t(Op::F64Const): {
        double c;
        if (!iter.readF64Const(&c)) {
          return false;
        }
        *literal = Some(LitVal(c));
        break;
      }
#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        if (!env->v128Enabled()) {
          return d.fail("v128 not enabled");
        }
        if (op.b1 != uint32_t(SimdOp::V128Const)) {
          return d.fail("unexpected initializer opcode");
        }
        V128 c;
        if (!iter.readV128Const(&c)) {
          return false;
        }
        *literal = Some(LitVal(c));
        break;
      }
#endif
      case uint16_t(Op::RefFunc): {
        uint32_t funcIndex;
        if (!iter.readRefFunc(&funcIndex)) {
          return false;
        }
        env->declareFuncExported(funcIndex, /* eager */ false,
                                 /* canRefFunc */ true);
        *literal = Nothing();
        break;
      }
      case uint16_t(Op::RefNull): {
        RefType type;
        if (!iter.readRefNull(&type)) {
          return false;
        }
        *literal = Some(LitVal(ValType(type)));
        break;
      }
#ifdef ENABLE_WASM_EXTENDED_CONST
      case uint16_t(Op::I32Add):
      case uint16_t(Op::I32Sub):
      case uint16_t(Op::I32Mul): {
        if (!env->extendedConstEnabled()) {
          return d.fail("unexpected initializer opcode");
        }
        if (!iter.readBinary(ValType::I32, &nothing, &nothing)) {
          return false;
        }
        *literal = Nothing();
        break;
      }
      case uint16_t(Op::I64Add):
      case uint16_t(Op::I64Sub):
      case uint16_t(Op::I64Mul): {
        if (!env->extendedConstEnabled()) {
          return d.fail("unexpected initializer opcode");
        }
        if (!iter.readBinary(ValType::I64, &nothing, &nothing)) {
          return false;
        }
        *literal = Nothing();
        break;
      }
#endif
      default: {
        return d.fail("unexpected initializer opcode");
      }
    }
  }
}

class MOZ_STACK_CLASS InitExprInterpreter {
 public:
  explicit InitExprInterpreter(JSContext* cx,
                               const ValVector& globalImportValues,
                               HandleWasmInstanceObject instanceObj)
      : features(FeatureArgs::build(cx, FeatureOptions())),
        stack(cx),
        globalImportValues(globalImportValues),
        instanceObj(cx, instanceObj) {}

  bool evaluate(Decoder& d);

  Val result() {
    MOZ_ASSERT(stack.length() == 1);
    return stack.popCopy();
  }

 private:
  FeatureArgs features;
  RootedValVector stack;
  const ValVector& globalImportValues;
  RootedWasmInstanceObject instanceObj;

  bool pushI32(int32_t c) { return stack.append(Val(uint32_t(c))); }
  bool pushI64(int64_t c) { return stack.append(Val(uint64_t(c))); }
  bool pushF32(float c) { return stack.append(Val(c)); }
  bool pushF64(double c) { return stack.append(Val(c)); }
  bool pushV128(V128 c) { return stack.append(Val(c)); }
  bool pushRef(ValType type, AnyRef ref) {
    return stack.append(Val(type, ref));
  }
  bool pushFuncRef(FuncRef ref) {
    return stack.append(Val(RefType::func(), ref));
  }

#ifdef ENABLE_WASM_EXTENDED_CONST
  int32_t popI32() {
    uint32_t result = stack.back().i32();
    stack.popBack();
    return int32_t(result);
  }
  int64_t popI64() {
    uint64_t result = stack.back().i64();
    stack.popBack();
    return int64_t(result);
  }
#endif

  bool evalGetGlobal(uint32_t index) {
    return stack.append(globalImportValues[index]);
  }
  bool evalI32Const(int32_t c) { return pushI32(c); }
  bool evalI64Const(int64_t c) { return pushI64(c); }
  bool evalF32Const(float c) { return pushF32(c); }
  bool evalF64Const(double c) { return pushF64(c); }
  bool evalV128Const(V128 c) { return pushV128(c); }
  bool evalRefFunc(uint32_t funcIndex) {
    void* fnref = Instance::refFunc(&instanceObj->instance(), funcIndex);
    if (fnref == AnyRef::invalid().forCompiledCode()) {
      return false;  // OOM, which has already been reported.
    }
    return pushFuncRef(FuncRef::fromCompiledCode(fnref));
  }
  bool evalRefNull(RefType type) { return pushRef(type, AnyRef::null()); }
#ifdef ENABLE_WASM_EXTENDED_CONST
  bool evalI32Add() {
    uint32_t a = popI32();
    uint32_t b = popI32();
    pushI32(a + b);
    return true;
  }
  bool evalI32Sub() {
    uint32_t a = popI32();
    uint32_t b = popI32();
    pushI32(a - b);
    return true;
  }
  bool evalI32Mul() {
    uint32_t a = popI32();
    uint32_t b = popI32();
    pushI32(a * b);
    return true;
  }
  bool evalI64Add() {
    uint64_t a = popI64();
    uint64_t b = popI64();
    pushI64(a + b);
    return true;
  }
  bool evalI64Sub() {
    uint64_t a = popI64();
    uint64_t b = popI64();
    pushI64(a - b);
    return true;
  }
  bool evalI64Mul() {
    uint64_t a = popI64();
    uint64_t b = popI64();
    pushI64(a * b);
    return true;
  }
#endif
};

bool InitExprInterpreter::evaluate(Decoder& d) {
#define CHECK(c)          \
  if (!(c)) return false; \
  break

  while (true) {
    OpBytes op;
    if (!d.readOp(&op)) {
      return false;
    }

    switch (op.b0) {
      case uint16_t(Op::End): {
        return true;
      }
      case uint16_t(Op::GetGlobal): {
        uint32_t index;
        if (!d.readGetGlobal(&index)) {
          return false;
        }
        CHECK(evalGetGlobal(index));
      }
      case uint16_t(Op::I32Const): {
        int32_t c;
        if (!d.readI32Const(&c)) {
          return false;
        }
        CHECK(evalI32Const(c));
      }
      case uint16_t(Op::I64Const): {
        int64_t c;
        if (!d.readI64Const(&c)) {
          return false;
        }
        CHECK(evalI64Const(c));
      }
      case uint16_t(Op::F32Const): {
        float c;
        if (!d.readF32Const(&c)) {
          return false;
        }
        CHECK(evalF32Const(c));
      }
      case uint16_t(Op::F64Const): {
        double c;
        if (!d.readF64Const(&c)) {
          return false;
        }
        CHECK(evalF64Const(c));
      }
#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        MOZ_RELEASE_ASSERT(op.b1 == uint32_t(SimdOp::V128Const));
        V128 c;
        if (!d.readV128Const(&c)) {
          return false;
        }
        CHECK(evalV128Const(c));
      }
#endif
      case uint16_t(Op::RefFunc): {
        uint32_t funcIndex;
        if (!d.readRefFunc(&funcIndex)) {
          return false;
        }
        CHECK(evalRefFunc(funcIndex));
      }
      case uint16_t(Op::RefNull): {
        RefType type;
        if (!d.readRefNull(features, &type)) {
          return false;
        }
        CHECK(evalRefNull(type));
      }
#ifdef ENABLE_WASM_EXTENDED_CONST
      case uint16_t(Op::I32Add): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI32Add());
      }
      case uint16_t(Op::I32Sub): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI32Sub());
      }
      case uint16_t(Op::I32Mul): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI32Mul());
      }
      case uint16_t(Op::I64Add): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI64Add());
      }
      case uint16_t(Op::I64Sub): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI64Sub());
      }
      case uint16_t(Op::I64Mul): {
        if (!d.readBinary()) {
          return false;
        }
        CHECK(evalI64Mul());
      }
#endif
      default: {
        MOZ_CRASH();
      }
    }
  }

#undef CHECK
}

bool InitExpr::decodeAndValidate(Decoder& d, ModuleEnvironment* env,
                                 ValType expected, InitExpr* expr) {
  Maybe<LitVal> literal = Nothing();
  const uint8_t* exprStart = d.currentPosition();
  if (!ValidateInitExpr(d, env, expected, &literal)) {
    return false;
  }
  const uint8_t* exprEnd = d.currentPosition();
  size_t exprSize = exprEnd - exprStart;

  MOZ_ASSERT(expr->kind_ == InitExprKind::None);
  expr->type_ = expected;

  if (literal) {
    expr->kind_ = InitExprKind::Literal;
    expr->literal_ = *literal;
    return true;
  }

  expr->kind_ = InitExprKind::Variable;
  return expr->bytecode_.reserve(exprSize) &&
         expr->bytecode_.append(exprStart, exprEnd);
}

bool InitExpr::evaluate(JSContext* cx, const ValVector& globalImportValues,
                        HandleWasmInstanceObject instanceObj,
                        MutableHandleVal result) const {
  MOZ_ASSERT(kind_ != InitExprKind::None);

  if (isLiteral()) {
    result.set(Val(literal()));
    return true;
  }

  UniqueChars error;
  Decoder d(bytecode_.begin(), bytecode_.end(), 0, &error);
  InitExprInterpreter interp(cx, globalImportValues, instanceObj);
  if (!interp.evaluate(d)) {
    // This expression should have been validated already. So we should only be
    // able to OOM, which is reported by having no error message.
    MOZ_RELEASE_ASSERT(!error);
    return false;
  }

  result.set(interp.result());
  return true;
}

bool InitExpr::clone(const InitExpr& src) {
  kind_ = src.kind_;
  MOZ_ASSERT(bytecode_.empty());
  if (!bytecode_.appendAll(src.bytecode_)) {
    return false;
  }
  literal_ = src.literal_;
  type_ = src.type_;
  return true;
}

size_t InitExpr::serializedSize() const {
  size_t size = sizeof(kind_) + sizeof(type_);
  switch (kind_) {
    case InitExprKind::Literal:
      size += sizeof(literal_);
      break;
    case InitExprKind::Variable:
      size += SerializedPodVectorSize(bytecode_);
      break;
    default:
      MOZ_CRASH();
  }
  return size;
}

uint8_t* InitExpr::serialize(uint8_t* cursor) const {
  cursor = WriteBytes(cursor, &kind_, sizeof(kind_));
  cursor = WriteBytes(cursor, &type_, sizeof(type_));
  switch (kind_) {
    case InitExprKind::Literal:
      cursor = WriteBytes(cursor, &literal_, sizeof(literal_));
      break;
    case InitExprKind::Variable:
      cursor = SerializePodVector(cursor, bytecode_);
      break;
    default:
      MOZ_CRASH();
  }
  return cursor;
}

const uint8_t* InitExpr::deserialize(const uint8_t* cursor) {
  if (!(cursor = ReadBytes(cursor, &kind_, sizeof(kind_))) ||
      !(cursor = ReadBytes(cursor, &type_, sizeof(type_)))) {
    return nullptr;
  }
  switch (kind_) {
    case InitExprKind::Literal:
      cursor = ReadBytes(cursor, &literal_, sizeof(literal_));
      break;
    case InitExprKind::Variable:
      cursor = DeserializePodVector(cursor, &bytecode_);
      break;
    default:
      MOZ_CRASH();
  }
  return cursor;
}

size_t InitExpr::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return bytecode_.sizeOfExcludingThis(mallocSizeOf);
}
