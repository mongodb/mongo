/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_wasm32_MoveEmitter_wasm32_h
#define jit_wasm32_MoveEmitter_wasm32_h

#include "mozilla/Assertions.h"

namespace js::jit {

class MacroAssemblerWasm32;
class MoveResolver;
struct Register;

class MoveEmitterWasm32 {
 public:
  explicit MoveEmitterWasm32(MacroAssemblerWasm32&) { MOZ_CRASH(); }
  void emit(const MoveResolver&) { MOZ_CRASH(); }
  void finish() { MOZ_CRASH(); }
  void setScratchRegister(Register) { MOZ_CRASH(); }
};

typedef MoveEmitterWasm32 MoveEmitter;

}  // namespace js::jit

#endif /* jit_wasm32_MoveEmitter_wasm32_h */
