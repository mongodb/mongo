/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_JumpList_h
#define frontend_JumpList_h

#include <stddef.h>  // ptrdiff_t

#include "frontend/BytecodeOffset.h"  // BytecodeOffset
#include "js/TypeDecls.h"             // jsbytecode

namespace js {
namespace frontend {

// Linked list of jump instructions that need to be patched. The linked list is
// stored in the bytes of the incomplete bytecode that will be patched, so no
// extra memory is needed, and patching the instructions destroys the list.
//
// Example:
//
//     JumpList brList;
//     if (!emitJump(JSOp::JumpIfFalse, &brList)) {
//         return false;
//     }
//     ...
//     JumpTarget label;
//     if (!emitJumpTarget(&label)) {
//         return false;
//     }
//     ...
//     if (!emitJump(JSOp::Goto, &brList)) {
//         return false;
//     }
//     ...
//     patchJumpsToTarget(brList, label);
//
//                      +-> (the delta is END_OF_LIST_DELTA (=0) for the last
//                      |    item)
//                      |
//                      |
//    JumpIfFalse .. <+ +                +-+   JumpIfFalse ..
//    ..              |                  |     ..
//  label:            |                  +-> label:
//    JumpTarget      |                  |     JumpTarget
//    ..              |                  |     ..
//    Goto .. <+ +----+                  +-+   Goto .. <+
//             |                                   |
//             |                                   |
//             +                                   +
//           brList                              brList
//
//       |                                  ^
//       +------- patchJumpsToTarget -------+
//

// Offset of a jump target instruction, used for patching jump instructions.
struct JumpTarget {
  BytecodeOffset offset = BytecodeOffset::invalidOffset();
};

struct JumpList {
  // Delta value for pre-patchJumpsToTarget that marks the end of the link.
  static const ptrdiff_t END_OF_LIST_DELTA = 0;

  // -1 is used to mark the end of jump lists.
  JumpList() : offset(BytecodeOffset::invalidOffset()) {}

  BytecodeOffset offset;

  // Add a jump instruction to the list.
  void push(jsbytecode* code, BytecodeOffset jumpOffset);

  // Patch all jump instructions in this list to jump to `target`.  This
  // clobbers the list.
  void patchAll(jsbytecode* code, JumpTarget target);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_JumpList_h */
