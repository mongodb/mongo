/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_OptionalEmitter_h
#define frontend_OptionalEmitter_h

#include "mozilla/Attributes.h"

#include "frontend/JumpList.h"
#include "frontend/TDZCheckCache.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for optional expressions.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `obj?.prop;`
//     OptionalEmitter oe(this);
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Get,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     oe.emitJumpShortCircuit();
//     poe.emitGet(atom_of_prop);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `delete obj?.prop;`
//     OptionalEmitter oe(this);
//     OptionalPropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Delete,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     oe.emitJumpShortCircuit();
//     poe.emitDelete(atom_of_prop);
//     oe.emitOptionalJumpTarget(JSOp:True);
//
//   `obj?.[key];`
//     OptionalEmitter oe(this);
//     ElemOpEmitter eoe(this,
//                       ElemOpEmitter::Kind::Get,
//                       ElemOpEmitter::ObjKind::Other);
//     eoe.prepareForObj();
//     emit(obj);
//     oe.emitJumpShortCircuit();
//     eoe.prepareForKey();
//     emit(key);
//     eoe.emitGet();
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `delete obj?.[key];`
//     OptionalEmitter oe(this);
//     ElemOpEmitter eoe(this,
//                       ElemOpEmitter::Kind::Delete,
//                       ElemOpEmitter::ObjKind::Other);
//     eoe.prepareForObj();
//     emit(obj);
//     oe.emitJumpShortCircuit();
//     eoe.prepareForKey();
//     emit(key);
//     eoe.emitDelete();
//     oe.emitOptionalJumpTarget(JSOp::True);
//
//   `print?.(arg);`
//     OptionalEmitter oe(this);
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.emitNameCallee(print);
//     cone.emitThis();
//     oe.emitShortCircuitForCall();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, offset_of_callee);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `callee.prop?.(arg1, arg2);`
//     OptionalEmitter oe(this);
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     PropOpEmitter& poe = cone.prepareForPropCallee(false);
//     ... emit `callee.prop` with `poe` here...
//     cone.emitThis();
//     oe.emitShortCircuitForCall();
//     cone.prepareForNonSpreadArguments();
//     emit(arg1);
//     emit(arg2);
//     cone.emitEnd(2, offset_of_callee);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `callee[key]?.(arg);`
//     OptionalEmitter oe(this);
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     ElemOpEmitter& eoe = cone.prepareForElemCallee(false);
//     ... emit `callee[key]` with `eoe` here...
//     cone.emitThis();
//     oe.emitShortCircuitForCall();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, offset_of_callee);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `(function() { ... })?.(arg);`
//     OptionalEmitter oe(this);
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.prepareForFunctionCallee();
//     emit(function);
//     cone.emitThis();
//     oe.emitShortCircuitForCall();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, offset_of_callee);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
//   `(a?b)();`
//     OptionalEmitter oe(this);
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.prepareForFunctionCallee();
//     emit(optionalChain);
//     cone.emitThis();
//     oe.emitOptionalJumpTarget(JSOp::Undefined,
//                               OptionalEmitter::Kind::Reference);
//     oe.emitShortCircuitForCall();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, offset_of_callee);
//     oe.emitOptionalJumpTarget(JSOp::Undefined);
//
class MOZ_RAII OptionalEmitter {
 public:
  OptionalEmitter(BytecodeEmitter* bce, int32_t initialDepth);

 private:
  BytecodeEmitter* bce_;

  TDZCheckCache tdzCache_;

  // jumptarget for ShortCircuiting code, which has null or undefined values
  JumpList jumpShortCircuit_;

  // jumpTarget for code that does not shortCircuit
  JumpList jumpFinish_;

  // jumpTarget for code that does not shortCircuit
  int32_t initialDepth_;

  // The state of this emitter.
  //
  // +-------+   emitJumpShortCircuit        +--------------+
  // | Start |-+---------------------------->| ShortCircuit |-----------+
  // +-------+ |                             +--------------+           |
  //    +----->|                                                        |
  //    |      | emitJumpShortCircuitForCall +---------------------+    v
  //    |      +---------------------------->| ShortCircuitForCall |--->+
  //    |                                    +---------------------+    |
  //    |                                                               |
  //     ---------------------------------------------------------------+
  //                                                                    |
  //                                                                    |
  // +------------------------------------------------------------------+
  // |
  // | emitOptionalJumpTarget +---------+
  // +----------------------->| JumpEnd |
  //                          +---------+
  //
#ifdef DEBUG
  enum class State {
    // The initial state.
    Start,

    // for shortcircuiting in most cases.
    ShortCircuit,

    // for shortcircuiting from references, which have two items on
    // the stack. For example function calls.
    ShortCircuitForCall,

    // internally used, end of the jump code
    JumpEnd
  };

  State state_ = State::Start;
#endif

 public:
  enum class Kind {
    // Requires two values on the stack
    Reference,
    // Requires one value on the stack
    Other
  };

  [[nodiscard]] bool emitJumpShortCircuit();
  [[nodiscard]] bool emitJumpShortCircuitForCall();

  // JSOp is the op code to be emitted, Kind is if we are dealing with a
  // reference (in which case we need two elements on the stack) or other value
  // (which needs one element on the stack)
  [[nodiscard]] bool emitOptionalJumpTarget(JSOp op, Kind kind = Kind::Other);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_OptionalEmitter_h */
