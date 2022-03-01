/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ForOfLoopControl_h
#define frontend_ForOfLoopControl_h

#include "mozilla/Maybe.h"  // mozilla::Maybe

#include <stdint.h>  // int32_t, uint32_t

#include "jsapi.h"  // CompletionKind

#include "frontend/BytecodeControlStructures.h"  // NestableControl, LoopControl
#include "frontend/BytecodeOffset.h"             // BytecodeOffset
#include "frontend/IteratorKind.h"               // IteratorKind
#include "frontend/TryEmitter.h"                 // TryEmitter

namespace js {
namespace frontend {

struct BytecodeEmitter;
class EmitterScope;

class ForOfLoopControl : public LoopControl {
  // The stack depth of the iterator.
  int32_t iterDepth_;

  // for-of loops, when throwing from non-iterator code (i.e. from the body
  // or from evaluating the LHS of the loop condition), need to call
  // IteratorClose.  This is done by enclosing non-iterator code with
  // try-catch and call IteratorClose in `catch` block.
  // If IteratorClose itself throws, we must not re-call IteratorClose. Since
  // non-local jumps like break and return call IteratorClose, whenever a
  // non-local jump is emitted, we must tell catch block not to perform
  // IteratorClose.
  //
  //   for (x of y) {
  //     // Operations for iterator (IteratorNext etc) are outside of
  //     // try-block.
  //     try {
  //       ...
  //       if (...) {
  //         // Before non-local jump, clear iterator on the stack to tell
  //         // catch block not to perform IteratorClose.
  //         tmpIterator = iterator;
  //         iterator = undefined;
  //         IteratorClose(tmpIterator, { break });
  //         break;
  //       }
  //       ...
  //     } catch (e) {
  //       // Just throw again when iterator is cleared by non-local jump.
  //       if (iterator === undefined)
  //         throw e;
  //       IteratorClose(iterator, { throw, e });
  //     }
  //   }
  mozilla::Maybe<TryEmitter> tryCatch_;

  // Used to track if any yields were emitted between calls to to
  // emitBeginCodeNeedingIteratorClose and emitEndCodeNeedingIteratorClose.
  uint32_t numYieldsAtBeginCodeNeedingIterClose_;

  bool allowSelfHosted_;

  IteratorKind iterKind_;

 public:
  ForOfLoopControl(BytecodeEmitter* bce, int32_t iterDepth,
                   bool allowSelfHosted, IteratorKind iterKind);

  [[nodiscard]] bool emitBeginCodeNeedingIteratorClose(BytecodeEmitter* bce);
  [[nodiscard]] bool emitEndCodeNeedingIteratorClose(BytecodeEmitter* bce);

  [[nodiscard]] bool emitIteratorCloseInInnermostScopeWithTryNote(
      BytecodeEmitter* bce,
      CompletionKind completionKind = CompletionKind::Normal);
  [[nodiscard]] bool emitIteratorCloseInScope(
      BytecodeEmitter* bce, EmitterScope& currentScope,
      CompletionKind completionKind = CompletionKind::Normal);

  [[nodiscard]] bool emitPrepareForNonLocalJumpFromScope(
      BytecodeEmitter* bce, EmitterScope& currentScope, bool isTarget,
      BytecodeOffset* tryNoteStart);
};
template <>
inline bool NestableControl::is<ForOfLoopControl>() const {
  return kind_ == StatementKind::ForOfLoop;
}

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ForOfLoopControl_h */
