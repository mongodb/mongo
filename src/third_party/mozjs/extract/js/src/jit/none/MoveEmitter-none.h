/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_MoveEmitter_none_h
#define jit_none_MoveEmitter_none_h

#include "mozilla/Assertions.h"

namespace js {
namespace jit {

class MacroAssemblerNone;
class MoveResolver;
struct Register;

class MoveEmitterNone {
 public:
  explicit MoveEmitterNone(MacroAssemblerNone&) { MOZ_CRASH(); }
  void emit(const MoveResolver&) { MOZ_CRASH(); }
  void finish() { MOZ_CRASH(); }
  void setScratchRegister(Register) { MOZ_CRASH(); }
};

typedef MoveEmitterNone MoveEmitter;

}  // namespace jit
}  // namespace js

#endif /* jit_none_MoveEmitter_none_h */
