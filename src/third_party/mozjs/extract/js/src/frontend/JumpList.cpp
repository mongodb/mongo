/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/JumpList.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "vm/BytecodeUtil.h"  // GET_JUMP_OFFSET, SET_JUMP_OFFSET, IsJumpOpcode

using namespace js;
using namespace js::frontend;

void JumpList::push(jsbytecode* code, BytecodeOffset jumpOffset) {
  if (!offset.valid()) {
    SET_JUMP_OFFSET(&code[jumpOffset.value()], END_OF_LIST_DELTA);
  } else {
    SET_JUMP_OFFSET(&code[jumpOffset.value()], (offset - jumpOffset).value());
  }
  offset = jumpOffset;
}

void JumpList::patchAll(jsbytecode* code, JumpTarget target) {
  if (!offset.valid()) {
    // This list is not used. Nothing to do.
    return;
  }

  BytecodeOffsetDiff delta;
  BytecodeOffset jumpOffset = offset;
  while (true) {
    jsbytecode* pc = &code[jumpOffset.value()];
    MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)));
    delta = BytecodeOffsetDiff(GET_JUMP_OFFSET(pc));
    MOZ_ASSERT(delta.value() == END_OF_LIST_DELTA || delta.value() < 0);
    BytecodeOffsetDiff span = target.offset - jumpOffset;
    SET_JUMP_OFFSET(pc, span.value());

    if (delta.value() == END_OF_LIST_DELTA) {
      break;
    }
    jumpOffset += delta;
  }
}
