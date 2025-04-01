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

#include "js/Value.h"

#include "wasm/WasmGcObject.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmUtility.h"
#include "wasm/WasmValidate.h"

#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;

class MOZ_STACK_CLASS InitExprInterpreter {
 public:
  explicit InitExprInterpreter(JSContext* cx,
                               Handle<WasmInstanceObject*> instanceObj)
      : features(FeatureArgs::build(cx, FeatureOptions())),
        stack(cx),
        instanceObj(cx, instanceObj),
        types(instanceObj->instance().metadata().types) {}

  bool evaluate(JSContext* cx, Decoder& d);

  Val result() {
    MOZ_ASSERT(stack.length() == 1);
    return stack.popCopy();
  }

 private:
  FeatureArgs features;
  RootedValVectorN<48> stack;
  Rooted<WasmInstanceObject*> instanceObj;
  SharedTypeContext types;

  Instance& instance() { return instanceObj->instance(); }

  [[nodiscard]] bool pushI32(int32_t c) {
    return stack.append(Val(uint32_t(c)));
  }
  [[nodiscard]] bool pushI64(int64_t c) {
    return stack.append(Val(uint64_t(c)));
  }
  [[nodiscard]] bool pushF32(float c) { return stack.append(Val(c)); }
  [[nodiscard]] bool pushF64(double c) { return stack.append(Val(c)); }
  [[nodiscard]] bool pushV128(V128 c) { return stack.append(Val(c)); }
  [[nodiscard]] bool pushRef(ValType type, AnyRef ref) {
    return stack.append(Val(type, ref));
  }
  [[nodiscard]] bool pushFuncRef(HandleFuncRef ref) {
    return stack.append(Val(RefType::func(), ref));
  }

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

  bool evalGlobalGet(JSContext* cx, uint32_t index) {
    RootedVal val(cx);
    instance().constantGlobalGet(index, &val);
    return stack.append(val);
  }
  bool evalI32Const(int32_t c) { return pushI32(c); }
  bool evalI64Const(int64_t c) { return pushI64(c); }
  bool evalF32Const(float c) { return pushF32(c); }
  bool evalF64Const(double c) { return pushF64(c); }
  bool evalV128Const(V128 c) { return pushV128(c); }
  bool evalRefFunc(JSContext* cx, uint32_t funcIndex) {
    RootedFuncRef func(cx, FuncRef::fromJSFunction(nullptr));
    if (!instance().constantRefFunc(funcIndex, &func)) {
      return false;
    }
    return pushFuncRef(func);
  }
  bool evalRefNull(RefType type) { return pushRef(type, AnyRef::null()); }
  bool evalI32Add() {
    uint32_t b = popI32();
    uint32_t a = popI32();
    return pushI32(a + b);
  }
  bool evalI32Sub() {
    uint32_t b = popI32();
    uint32_t a = popI32();
    return pushI32(a - b);
  }
  bool evalI32Mul() {
    uint32_t b = popI32();
    uint32_t a = popI32();
    return pushI32(a * b);
  }
  bool evalI64Add() {
    uint64_t b = popI64();
    uint64_t a = popI64();
    return pushI64(a + b);
  }
  bool evalI64Sub() {
    uint64_t b = popI64();
    uint64_t a = popI64();
    return pushI64(a - b);
  }
  bool evalI64Mul() {
    uint64_t b = popI64();
    uint64_t a = popI64();
    return pushI64(a * b);
  }
#ifdef ENABLE_WASM_GC
  bool evalStructNew(JSContext* cx, uint32_t typeIndex) {
    const TypeDef& typeDef = instance().metadata().types->type(typeIndex);
    const StructType& structType = typeDef.structType();

    Rooted<WasmStructObject*> structObj(
        cx, instance().constantStructNewDefault(cx, typeIndex));
    if (!structObj) {
      return false;
    }

    uint32_t numFields = structType.fields_.length();
    for (uint32_t forwardIndex = 0; forwardIndex < numFields; forwardIndex++) {
      uint32_t reverseIndex = numFields - forwardIndex - 1;
      const Val& val = stack.back();
      structObj->storeVal(val, reverseIndex);
      stack.popBack();
    }

    return pushRef(RefType::fromTypeDef(&typeDef, false),
                   AnyRef::fromJSObject(*structObj));
  }

  bool evalStructNewDefault(JSContext* cx, uint32_t typeIndex) {
    Rooted<WasmStructObject*> structObj(
        cx, instance().constantStructNewDefault(cx, typeIndex));
    if (!structObj) {
      return false;
    }

    const TypeDef& typeDef = instance().metadata().types->type(typeIndex);
    return pushRef(RefType::fromTypeDef(&typeDef, false),
                   AnyRef::fromJSObject(*structObj));
  }

  bool evalArrayNew(JSContext* cx, uint32_t typeIndex) {
    uint32_t numElements = popI32();
    Rooted<WasmArrayObject*> arrayObj(
        cx, instance().constantArrayNewDefault(cx, typeIndex, numElements));
    if (!arrayObj) {
      return false;
    }

    const Val& val = stack.back();
    arrayObj->fillVal(val, 0, numElements);
    stack.popBack();

    const TypeDef& typeDef = instance().metadata().types->type(typeIndex);
    return pushRef(RefType::fromTypeDef(&typeDef, false),
                   AnyRef::fromJSObject(*arrayObj));
  }

  bool evalArrayNewDefault(JSContext* cx, uint32_t typeIndex) {
    uint32_t numElements = popI32();
    Rooted<WasmArrayObject*> arrayObj(
        cx, instance().constantArrayNewDefault(cx, typeIndex, numElements));
    if (!arrayObj) {
      return false;
    }

    const TypeDef& typeDef = instance().metadata().types->type(typeIndex);
    return pushRef(RefType::fromTypeDef(&typeDef, false),
                   AnyRef::fromJSObject(*arrayObj));
  }

  bool evalArrayNewFixed(JSContext* cx, uint32_t typeIndex,
                         uint32_t numElements) {
    Rooted<WasmArrayObject*> arrayObj(
        cx, instance().constantArrayNewDefault(cx, typeIndex, numElements));
    if (!arrayObj) {
      return false;
    }

    for (uint32_t forwardIndex = 0; forwardIndex < numElements;
         forwardIndex++) {
      uint32_t reverseIndex = numElements - forwardIndex - 1;
      const Val& val = stack.back();
      arrayObj->storeVal(val, reverseIndex);
      stack.popBack();
    }

    const TypeDef& typeDef = instance().metadata().types->type(typeIndex);
    return pushRef(RefType::fromTypeDef(&typeDef, false),
                   AnyRef::fromJSObject(*arrayObj));
  }

  bool evalI31New(JSContext* cx) {
    uint32_t value = stack.back().i32();
    stack.popBack();
    return pushRef(RefType::i31().asNonNullable(),
                   AnyRef::fromUint32Truncate(value));
  }

  bool evalAnyConvertExtern(JSContext* cx) {
    AnyRef ref = stack.back().ref();
    stack.popBack();
    return pushRef(RefType::extern_(), ref);
  }

  bool evalExternConvertAny(JSContext* cx) {
    AnyRef ref = stack.back().ref();
    stack.popBack();
    return pushRef(RefType::any(), ref);
  }
#endif  // ENABLE_WASM_GC
};

bool InitExprInterpreter::evaluate(JSContext* cx, Decoder& d) {
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
      case uint16_t(Op::GlobalGet): {
        uint32_t index;
        if (!d.readGlobalIndex(&index)) {
          return false;
        }
        CHECK(evalGlobalGet(cx, index));
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
        if (!d.readFuncIndex(&funcIndex)) {
          return false;
        }
        CHECK(evalRefFunc(cx, funcIndex));
      }
      case uint16_t(Op::RefNull): {
        RefType type;
        if (!d.readRefNull(*types, features, &type)) {
          return false;
        }
        CHECK(evalRefNull(type));
      }
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
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::GcPrefix): {
        switch (op.b1) {
          case uint32_t(GcOp::StructNew): {
            uint32_t typeIndex;
            if (!d.readTypeIndex(&typeIndex)) {
              return false;
            }
            CHECK(evalStructNew(cx, typeIndex));
          }
          case uint32_t(GcOp::StructNewDefault): {
            uint32_t typeIndex;
            if (!d.readTypeIndex(&typeIndex)) {
              return false;
            }
            CHECK(evalStructNewDefault(cx, typeIndex));
          }
          case uint32_t(GcOp::ArrayNew): {
            uint32_t typeIndex;
            if (!d.readTypeIndex(&typeIndex)) {
              return false;
            }
            CHECK(evalArrayNew(cx, typeIndex));
          }
          case uint32_t(GcOp::ArrayNewFixed): {
            uint32_t typeIndex, len;
            if (!d.readTypeIndex(&typeIndex)) {
              return false;
            }
            if (!d.readVarU32(&len)) {
              return false;
            }
            CHECK(evalArrayNewFixed(cx, typeIndex, len));
          }
          case uint32_t(GcOp::ArrayNewDefault): {
            uint32_t typeIndex;
            if (!d.readTypeIndex(&typeIndex)) {
              return false;
            }
            CHECK(evalArrayNewDefault(cx, typeIndex));
          }
          case uint32_t(GcOp::RefI31): {
            CHECK(evalI31New(cx));
          }
          case uint32_t(GcOp::AnyConvertExtern): {
            CHECK(evalAnyConvertExtern(cx));
          }
          case uint32_t(GcOp::ExternConvertAny): {
            CHECK(evalExternConvertAny(cx));
          }
          default: {
            MOZ_CRASH();
          }
        }
        break;
      }
#endif
      default: {
        MOZ_CRASH();
      }
    }
  }

#undef CHECK
}

bool wasm::DecodeConstantExpression(Decoder& d, ModuleEnvironment* env,
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

    Nothing nothing;
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
      case uint16_t(Op::GlobalGet): {
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
        if (!env->simdAvailable()) {
          return d.fail("v128 not enabled");
        }
        if (op.b1 != uint32_t(SimdOp::V128Const)) {
          return iter.unrecognizedOpcode(&op);
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
      case uint16_t(Op::I32Add):
      case uint16_t(Op::I32Sub):
      case uint16_t(Op::I32Mul): {
        if (!iter.readBinary(ValType::I32, &nothing, &nothing)) {
          return false;
        }
        *literal = Nothing();
        break;
      }
      case uint16_t(Op::I64Add):
      case uint16_t(Op::I64Sub):
      case uint16_t(Op::I64Mul): {
        if (!iter.readBinary(ValType::I64, &nothing, &nothing)) {
          return false;
        }
        *literal = Nothing();
        break;
      }
#ifdef ENABLE_WASM_GC
      case uint16_t(Op::GcPrefix): {
        if (!env->gcEnabled()) {
          return iter.unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(GcOp::StructNew): {
            uint32_t typeIndex;
            if (!iter.readStructNew(&typeIndex, &nothings)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::StructNewDefault): {
            uint32_t typeIndex;
            if (!iter.readStructNewDefault(&typeIndex)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::ArrayNew): {
            uint32_t typeIndex;
            if (!iter.readArrayNew(&typeIndex, &nothing, &nothing)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::ArrayNewFixed): {
            uint32_t typeIndex, len;
            if (!iter.readArrayNewFixed(&typeIndex, &len, &nothings)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::ArrayNewDefault): {
            uint32_t typeIndex;
            if (!iter.readArrayNewDefault(&typeIndex, &nothing)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::RefI31): {
            Nothing value;
            if (!iter.readConversion(ValType::I32,
                                     ValType(RefType::i31().asNonNullable()),
                                     &value)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::AnyConvertExtern): {
            Nothing value;
            if (!iter.readRefConversion(RefType::extern_(), RefType::any(),
                                        &value)) {
              return false;
            }
            break;
          }
          case uint32_t(GcOp::ExternConvertAny): {
            Nothing value;
            if (!iter.readRefConversion(RefType::any(), RefType::extern_(),
                                        &value)) {
              return false;
            }
            break;
          }
          default: {
            return iter.unrecognizedOpcode(&op);
          }
        }
        *literal = Nothing();
        break;
      }
#endif
      default: {
        return iter.unrecognizedOpcode(&op);
      }
    }
  }
}

bool InitExpr::decodeAndValidate(Decoder& d, ModuleEnvironment* env,
                                 ValType expected, InitExpr* expr) {
  Maybe<LitVal> literal = Nothing();
  const uint8_t* exprStart = d.currentPosition();
  if (!DecodeConstantExpression(d, env, expected, &literal)) {
    return false;
  }
  const uint8_t* exprEnd = d.currentPosition();
  size_t exprSize = exprEnd - exprStart;

  MOZ_ASSERT(expr->kind_ == InitExprKind::None);
  expr->type_ = expected;

  if (literal) {
    literal->unsafeSetType(expected);
    expr->kind_ = InitExprKind::Literal;
    expr->literal_ = *literal;
    return true;
  }

  expr->kind_ = InitExprKind::Variable;
  return expr->bytecode_.reserve(exprSize) &&
         expr->bytecode_.append(exprStart, exprEnd);
}

/* static */ bool InitExpr::decodeAndEvaluate(
    JSContext* cx, Handle<WasmInstanceObject*> instanceObj, Decoder& d,
    ValType expectedType, MutableHandleVal result) {
  InitExprInterpreter interp(cx, instanceObj);
  if (!interp.evaluate(cx, d)) {
    return false;
  }

  Val interpResult = interp.result();
  // The interpreter evaluation stack does not track the precise type of values.
  // Users of the result expect the precise type though, so we need to overwrite
  // it with the one we validated with.
  interpResult.unsafeSetType(expectedType);
  result.set(interpResult);
  return true;
}

bool InitExpr::evaluate(JSContext* cx, Handle<WasmInstanceObject*> instanceObj,
                        MutableHandleVal result) const {
  MOZ_ASSERT(kind_ != InitExprKind::None);

  if (isLiteral()) {
    result.set(Val(literal()));
    return true;
  }

  UniqueChars error;
  Decoder d(bytecode_.begin(), bytecode_.end(), 0, &error);
  if (!decodeAndEvaluate(cx, instanceObj, d, type_, result)) {
    // This expression should have been validated already. So we should only be
    // able to OOM, which is reported by having no error message.
    MOZ_RELEASE_ASSERT(!error);
    return false;
  }

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

size_t InitExpr::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return bytecode_.sizeOfExcludingThis(mallocSizeOf);
}
