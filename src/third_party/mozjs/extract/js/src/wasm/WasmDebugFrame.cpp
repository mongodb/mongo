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

#include "wasm/WasmDebugFrame.h"

#include "vm/EnvironmentObject.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmStubs.h"

#include "vm/NativeObject-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

/* static */
DebugFrame* DebugFrame::from(Frame* fp) {
  MOZ_ASSERT(GetNearestEffectiveInstance(fp)->code().metadata().debugEnabled);
  auto* df =
      reinterpret_cast<DebugFrame*>((uint8_t*)fp - DebugFrame::offsetOfFrame());
  MOZ_ASSERT(GetNearestEffectiveInstance(fp) == df->instance());
  return df;
}

void DebugFrame::alignmentStaticAsserts() {
  // VS2017 doesn't consider offsetOfFrame() to be a constexpr, so we have
  // to use offsetof directly. These asserts can't be at class-level
  // because the type is incomplete.

  static_assert(WasmStackAlignment >= Alignment,
                "Aligned by ABI before pushing DebugFrame");
#ifndef JS_CODEGEN_NONE
  static_assert((offsetof(DebugFrame, frame_) + sizeof(Frame)) % Alignment == 0,
                "Aligned after pushing DebugFrame");
#endif
#ifdef JS_CODEGEN_ARM64
  // This constraint may or may not be necessary.  If you hit this because
  // you've changed the frame size then feel free to remove it, but be extra
  // aware of possible problems.
  static_assert(sizeof(DebugFrame) % 16 == 0, "ARM64 SP alignment");
#endif
}

Instance* DebugFrame::instance() {
  return GetNearestEffectiveInstance(&frame_);
}

const Instance* DebugFrame::instance() const {
  return GetNearestEffectiveInstance(&frame_);
}

GlobalObject* DebugFrame::global() { return &instance()->object()->global(); }

bool DebugFrame::hasGlobal(const GlobalObject* global) const {
  return global == &instance()->objectUnbarriered()->global();
}

JSObject* DebugFrame::environmentChain() {
  return &global()->lexicalEnvironment();
}

bool DebugFrame::getLocal(uint32_t localIndex, MutableHandleValue vp) {
  ValTypeVector locals;
  size_t argsLength;
  StackResults stackResults;
  if (!instance()->debug().debugGetLocalTypes(funcIndex(), &locals, &argsLength,
                                              &stackResults)) {
    return false;
  }

  ValTypeVector args;
  MOZ_ASSERT(argsLength <= locals.length());
  if (!args.append(locals.begin(), argsLength)) {
    return false;
  }
  ArgTypeVector abiArgs(args, stackResults);

  BaseLocalIter iter(locals, abiArgs, /* debugEnabled = */ true);
  while (!iter.done() && iter.index() < localIndex) {
    iter++;
  }
  MOZ_ALWAYS_TRUE(!iter.done());

  uint8_t* frame = static_cast<uint8_t*>((void*)this) + offsetOfFrame();
  void* dataPtr = frame - iter.frameOffset();
  switch (iter.mirType()) {
    case jit::MIRType::Int32:
      vp.set(Int32Value(*static_cast<int32_t*>(dataPtr)));
      break;
    case jit::MIRType::Int64:
      // Just display as a Number; it's ok if we lose some precision
      vp.set(NumberValue((double)*static_cast<int64_t*>(dataPtr)));
      break;
    case jit::MIRType::Float32:
      vp.set(NumberValue(JS::CanonicalizeNaN(*static_cast<float*>(dataPtr))));
      break;
    case jit::MIRType::Double:
      vp.set(NumberValue(JS::CanonicalizeNaN(*static_cast<double*>(dataPtr))));
      break;
    case jit::MIRType::WasmAnyRef:
      vp.set(((AnyRef*)dataPtr)->toJSValue());
      break;
#ifdef ENABLE_WASM_SIMD
    case jit::MIRType::Simd128:
      vp.set(NumberValue(0));
      break;
#endif
    default:
      MOZ_CRASH("local type");
  }
  return true;
}

bool DebugFrame::updateReturnJSValue(JSContext* cx) {
  MutableHandleValue rval =
      MutableHandleValue::fromMarkedLocation(&cachedReturnJSValue_);
  rval.setUndefined();
  flags_.hasCachedReturnJSValue = true;
  ResultType resultType = ResultType::Vector(
      instance()->metadata().debugFuncType(funcIndex()).results());
  Maybe<char*> stackResultsLoc;
  if (ABIResultIter::HasStackResults(resultType)) {
    stackResultsLoc = Some(static_cast<char*>(stackResultsPointer_));
  }
  DebugCodegen(DebugChannel::Function,
               "wasm-function[%d] updateReturnJSValue [", funcIndex());
  bool ok =
      ResultsToJSValue(cx, resultType, registerResults_, stackResultsLoc, rval);
  DebugCodegen(DebugChannel::Function, "]\n");
  return ok;
}

void DebugFrame::discardReturnJSValue() {
  MutableHandleValue rval =
      MutableHandleValue::fromMarkedLocation(&cachedReturnJSValue_);
  rval.setMagic(JS_OPTIMIZED_OUT);
  flags_.hasCachedReturnJSValue = true;
}

HandleValue DebugFrame::returnValue() const {
  MOZ_ASSERT(flags_.hasCachedReturnJSValue);
  return HandleValue::fromMarkedLocation(&cachedReturnJSValue_);
}

void DebugFrame::clearReturnJSValue() {
  flags_.hasCachedReturnJSValue = true;
  cachedReturnJSValue_.setUndefined();
}

void DebugFrame::observe(JSContext* cx) {
  if (!flags_.observing) {
    instance()->debug().adjustEnterAndLeaveFrameTrapsState(
        cx, instance(), /* enabled = */ true);
    flags_.observing = true;
  }
}

void DebugFrame::leave(JSContext* cx) {
  if (flags_.observing) {
    instance()->debug().adjustEnterAndLeaveFrameTrapsState(
        cx, instance(), /* enabled = */ false);
    flags_.observing = false;
  }
}
