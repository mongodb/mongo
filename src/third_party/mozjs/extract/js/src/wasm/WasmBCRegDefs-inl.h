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

// This is an INTERNAL header for Wasm baseline compiler: inline BaseCompiler
// methods that don't fit in any other group in particular.

#ifndef wasm_wasm_baseline_reg_defs_inl_h
#define wasm_wasm_baseline_reg_defs_inl_h

namespace js {
namespace wasm {
// TODO / OPTIMIZE (Bug 1316802): Do not sync everything on allocation
// failure, only as much as we need.

RegI32 BaseRegAlloc::needI32() {
  if (!hasGPR()) {
    bc->sync();
  }
  return RegI32(allocGPR());
}

void BaseRegAlloc::needI32(RegI32 specific) {
  if (!isAvailableI32(specific)) {
    bc->sync();
  }
  allocGPR(specific);
}

RegI64 BaseRegAlloc::needI64() {
  if (!hasGPR64()) {
    bc->sync();
  }
  return RegI64(allocInt64());
}

void BaseRegAlloc::needI64(RegI64 specific) {
  if (!isAvailableI64(specific)) {
    bc->sync();
  }
  allocInt64(specific);
}

RegRef BaseRegAlloc::needRef() {
  if (!hasGPR()) {
    bc->sync();
  }
  return RegRef(allocGPR());
}

void BaseRegAlloc::needRef(RegRef specific) {
  if (!isAvailableRef(specific)) {
    bc->sync();
  }
  allocGPR(specific);
}

RegPtr BaseRegAlloc::needPtr() {
  if (!hasGPR()) {
    bc->sync();
  }
  return RegPtr(allocGPR());
}

void BaseRegAlloc::needPtr(RegPtr specific) {
  if (!isAvailablePtr(specific)) {
    bc->sync();
  }
  allocGPR(specific);
}

RegPtr BaseRegAlloc::needTempPtr(RegPtr fallback, bool* saved) {
  if (hasGPR()) {
    *saved = false;
    return RegPtr(allocGPR());
  }
  *saved = true;
  bc->saveTempPtr(fallback);
  MOZ_ASSERT(isAvailablePtr(fallback));
  allocGPR(fallback);
  return RegPtr(fallback);
}

RegF32 BaseRegAlloc::needF32() {
  if (!hasFPU<MIRType::Float32>()) {
    bc->sync();
  }
  return RegF32(allocFPU<MIRType::Float32>());
}

void BaseRegAlloc::needF32(RegF32 specific) {
  if (!isAvailableF32(specific)) {
    bc->sync();
  }
  allocFPU(specific);
}

RegF64 BaseRegAlloc::needF64() {
  if (!hasFPU<MIRType::Double>()) {
    bc->sync();
  }
  return RegF64(allocFPU<MIRType::Double>());
}

void BaseRegAlloc::needF64(RegF64 specific) {
  if (!isAvailableF64(specific)) {
    bc->sync();
  }
  allocFPU(specific);
}

#ifdef ENABLE_WASM_SIMD
RegV128 BaseRegAlloc::needV128() {
  if (!hasFPU<MIRType::Simd128>()) {
    bc->sync();
  }
  return RegV128(allocFPU<MIRType::Simd128>());
}

void BaseRegAlloc::needV128(RegV128 specific) {
  if (!isAvailableV128(specific)) {
    bc->sync();
  }
  allocFPU(specific);
}
#endif

void BaseRegAlloc::freeI32(RegI32 r) { freeGPR(r); }

void BaseRegAlloc::freeI64(RegI64 r) { freeInt64(r); }

void BaseRegAlloc::freeRef(RegRef r) { freeGPR(r); }

void BaseRegAlloc::freePtr(RegPtr r) { freeGPR(r); }

void BaseRegAlloc::freeF64(RegF64 r) { freeFPU(r); }

void BaseRegAlloc::freeF32(RegF32 r) { freeFPU(r); }

#ifdef ENABLE_WASM_SIMD
void BaseRegAlloc::freeV128(RegV128 r) { freeFPU(r); }
#endif

void BaseRegAlloc::freeTempPtr(RegPtr r, bool saved) {
  freePtr(r);
  if (saved) {
    bc->restoreTempPtr(r);
    MOZ_ASSERT(!isAvailablePtr(r));
  }
}

#ifdef JS_CODEGEN_ARM
RegI64 BaseRegAlloc::needI64Pair() {
  if (!hasGPRPair()) {
    bc->sync();
  }
  Register low, high;
  allocGPRPair(&low, &high);
  return RegI64(Register64(high, low));
}
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_reg_defs_inl_h
