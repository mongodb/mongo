/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TryEmitter_h
#define frontend_TryEmitter_h

#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe, mozilla::Nothing

#include <stdint.h>  // uint32_t

#include "frontend/BytecodeControlStructures.h"  // TryFinallyControl
#include "frontend/BytecodeOffset.h"             // BytecodeOffset
#include "frontend/JumpList.h"                   // JumpList, JumpTarget

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for blocks like try-catch-finally.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `try { try_block } catch (ex) { catch_block }`
//     TryEmitter tryCatch(this, TryEmitter::Kind::TryCatch,
//                         TryEmitter::ControlKind::Syntactic);
//     tryCatch.emitTry();
//     emit(try_block);
//     tryCatch.emitCatch();
//     emit(catch_block); // Pending exception is on stack
//     tryCatch.emitEnd();
//
//   `try { try_block } finally { finally_block }`
//     TryEmitter tryCatch(this, TryEmitter::Kind::TryFinally,
//                         TryEmitter::ControlKind::Syntactic);
//     tryCatch.emitTry();
//     emit(try_block);
//     // finally_pos: The "{" character's position in the source code text.
//     tryCatch.emitFinally(Some(finally_pos));
//     emit(finally_block);
//     tryCatch.emitEnd();
//
//   `try { try_block } catch (ex) {catch_block} finally { finally_block }`
//     TryEmitter tryCatch(this, TryEmitter::Kind::TryCatchFinally,
//                         TryEmitter::ControlKind::Syntactic);
//     tryCatch.emitTry();
//     emit(try_block);
//     tryCatch.emitCatch();
//     emit(catch_block);
//     tryCatch.emitFinally(Some(finally_pos));
//     emit(finally_block);
//     tryCatch.emitEnd();
//
class MOZ_STACK_CLASS TryEmitter {
 public:
  enum class Kind { TryCatch, TryCatchFinally, TryFinally };

  // Syntactic try-catch-finally and internally used non-syntactic
  // try-catch-finally behave differently for 2 points.
  //
  // The first one is whether TryFinallyControl is used or not.
  // See the comment for `controlInfo_`.
  //
  // The second one is whether the catch and finally blocks handle the frame's
  // return value.  For syntactic try-catch-finally, the bytecode marked with
  // "*" are emitted to clear return value with `undefined` before the catch
  // block and the finally block, and also to save/restore the return value
  // before/after the finally block. Note that these instructions are not
  // emitted for noScriptRval scripts that don't track the return value.
  //
  //     JSOp::Try offsetOf(jumpToEnd)
  //
  //     try_body...
  //
  //     JSOp::Goto finally
  //     JSOp::JumpTarget
  //   jumpToEnd:
  //     JSOp::Goto end:
  //
  //   catch:
  //     JSOp::JumpTarget
  //   * JSOp::Undefined
  //   * JSOp::SetRval
  //
  //     catch_body...
  //
  //     JSOp::Goto finally
  //     JSOp::JumpTarget
  //     JSOp::Goto end
  //
  //   finally:
  //     JSOp::JumpTarget
  //   * JSOp::GetRval
  //   * JSOp::Undefined
  //   * JSOp::SetRval
  //
  //     finally_body...
  //
  //   * JSOp::SetRval
  //     JSOp::Nop
  //
  //   end:
  //     JSOp::JumpTarget
  //
  // For syntactic try-catch-finally, Syntactic should be used.
  // For non-syntactic try-catch-finally, NonSyntactic should be used.
  enum class ControlKind { Syntactic, NonSyntactic };

 private:
  BytecodeEmitter* bce_;
  Kind kind_;
  ControlKind controlKind_;

  // Tracks jumps to the finally block for later fixup.
  //
  // When a finally block is active, non-local jumps (including
  // jumps-over-catches) result in a goto being written into the bytecode
  // stream and fixed-up later.
  //
  // For non-syntactic try-catch-finally, all that handling is skipped.
  // The non-syntactic try-catch-finally must:
  //   * have only one catch block
  //   * have JSOp::Goto at the end of catch-block
  //   * have no non-local-jump
  //   * don't use finally block for normal completion of try-block and
  //     catch-block
  //
  // Additionally, a finally block may be emitted for non-syntactic
  // try-catch-finally, even if the kind is TryCatch, because JSOp::Goto is
  // not emitted.
  mozilla::Maybe<TryFinallyControl> controlInfo_;

  // The stack depth before emitting JSOp::Try.
  int depth_;

  // The offset of the JSOp::Try op.
  BytecodeOffset tryOpOffset_;

  // JSOp::JumpTarget after the entire try-catch-finally block.
  JumpList catchAndFinallyJump_;

  // The offset of JSOp::Goto at the end of the try block.
  JumpTarget tryEnd_;

  // The offset of JSOp::JumpTarget at the beginning of the finally block.
  JumpTarget finallyStart_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ emitTry +-----+   emitCatch +-------+      emitEnd  +-----+
  // | Start |-------->| Try |-+---------->| Catch |-+->+--------->| End |
  // +-------+         +-----+ |           +-------+ |  ^          +-----+
  //                           |                     |  |
  //                           |  +------------------+  +----+
  //                           |  |                          |
  //                           |  v emitFinally +---------+  |
  //                           +->+------------>| Finally |--+
  //                                            +---------+
  enum class State {
    // The initial state.
    Start,

    // After calling emitTry.
    Try,

    // After calling emitCatch.
    Catch,

    // After calling emitFinally.
    Finally,

    // After calling emitEnd.
    End
  };
  State state_;
#endif

  bool hasCatch() const {
    return kind_ == Kind::TryCatch || kind_ == Kind::TryCatchFinally;
  }
  bool hasFinally() const {
    return kind_ == Kind::TryCatchFinally || kind_ == Kind::TryFinally;
  }

  BytecodeOffset offsetAfterTryOp() const {
    return tryOpOffset_ + BytecodeOffsetDiff(JSOpLength_Try);
  }

  // Returns true if catch and finally blocks should handle the frame's
  // return value.
  bool shouldUpdateRval() const;

  // Jump to the finally block. After the finally block executes,
  // fall through to the code following the finally block.
  [[nodiscard]] bool emitJumpToFinallyWithFallthrough();

 public:
  TryEmitter(BytecodeEmitter* bce, Kind kind, ControlKind controlKind);

  [[nodiscard]] bool emitTry();

  enum class ExceptionStack : bool {
    /**
     * Push only the pending exception value.
     */
    No,

    /**
     * Push the pending exception value and its stack.
     */
    Yes,
  };

  [[nodiscard]] bool emitCatch(ExceptionStack stack = ExceptionStack::No);

  // If `finallyPos` is specified, it's an offset of the finally block's
  // "{" character in the source code text, to improve line:column number in
  // the error reporting.
  // For non-syntactic try-catch-finally, `finallyPos` can be omitted.
  [[nodiscard]] bool emitFinally(
      const mozilla::Maybe<uint32_t>& finallyPos = mozilla::Nothing());

  [[nodiscard]] bool emitEnd();

 private:
  [[nodiscard]] bool emitTryEnd();
  [[nodiscard]] bool emitCatchEnd();
  [[nodiscard]] bool emitFinallyEnd();
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_TryEmitter_h */
