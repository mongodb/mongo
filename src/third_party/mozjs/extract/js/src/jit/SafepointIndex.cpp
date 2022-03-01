/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/SafepointIndex-inl.h"

#include "jit/MacroAssembler.h"

namespace js::jit {

uint32_t OsiIndex::returnPointDisplacement() const {
  // In general, pointer arithmetic on code is bad, but in this case,
  // getting the return address from a call instruction, stepping over pools
  // would be wrong.
  return callPointDisplacement_ + Assembler::PatchWrite_NearCallSize();
}

}  // namespace js::jit
