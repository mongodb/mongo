/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_CallOrNewEmitter_h
#define frontend_CallOrNewEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stdint.h>

#include "frontend/ElemOpEmitter.h"
#include "frontend/IfEmitter.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "frontend/PrivateOpEmitter.h"
#include "frontend/PropOpEmitter.h"
#include "frontend/ValueUsage.h"
#include "js/TypeDecls.h"
#include "vm/BytecodeUtil.h"
#include "vm/Opcodes.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for call or new expression.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `print(arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.emitNameCallee(print);
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `callee.prop(arg1, arg2);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     PropOpEmitter& poe = cone.prepareForPropCallee(false);
//     ... emit `callee.prop` with `poe` here...
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg1);
//     emit(arg2);
//     cone.emitEnd(2, Some(offset_of_callee));
//
//   `callee[key](arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     ElemOpEmitter& eoe = cone.prepareForElemCallee(false);
//     ... emit `callee[key]` with `eoe` here...
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `callee.#method(arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     PrivateOpEmitter& xoe = cone.prepareForPrivateCallee();
//     ... emit `callee.#method` with `xoe` here...
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `(function() { ... })(arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.prepareForFunctionCallee();
//     emit(function);
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `super(arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.emitSuperCallee();
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `(some_other_expression)(arg);`
//     CallOrNewEmitter cone(this, JSOp::Call,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.prepareForOtherCallee();
//     emit(some_other_expression);
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `print(...arg);`
//     CallOrNewEmitter cone(this, JSOp::SpreadCall,
//                           CallOrNewEmitter::ArgumentsKind::SingleSpread,
//                           ValueUsage::WantValue);
//     cone.emitNameCallee(print);
//     cone.emitThis();
//     if (cone.wantSpreadOperand()) {
//       emit(arg)
//     }
//     cone.emitSpreadArgumentsTest();
//     emit([...arg]);
//     cone.emitEnd(1, Some(offset_of_callee));
//
//   `new f(arg);`
//     CallOrNewEmitter cone(this, JSOp::New,
//                           CallOrNewEmitter::ArgumentsKind::Other,
//                           ValueUsage::WantValue);
//     cone.emitNameCallee(f);
//     cone.emitThis();
//     cone.prepareForNonSpreadArguments();
//     emit(arg);
//     cone.emitEnd(1, Some(offset_of_callee));
//
class MOZ_STACK_CLASS CallOrNewEmitter {
 public:
  enum class ArgumentsKind {
    Other,

    // Specify this for the following case:
    //
    //   g(...input);
    //
    // This enables optimization to avoid allocating an intermediate array
    // for spread operation.
    //
    // wantSpreadOperand() returns true when this is specified.
    SingleSpread,

    // Used for default derived class constructors:
    //
    //   constructor(...args) {
    //      super(...args);
    //   }
    //
    // The rest-parameter is directly passed through to the `super` call without
    // using the iteration protocol.
    PassthroughRest,
  };

 private:
  BytecodeEmitter* bce_;

  // The opcode for the call or new.
  JSOp op_;

  // Whether the call is a spread call with single parameter or not.
  // See the comment in emitSpreadArgumentsTest for more details.
  ArgumentsKind argumentsKind_;

  // The branch for spread call optimization.
  mozilla::Maybe<InternalIfEmitter> ifNotOptimizable_;

  mozilla::Maybe<PropOpEmitter> poe_;
  mozilla::Maybe<ElemOpEmitter> eoe_;
  mozilla::Maybe<PrivateOpEmitter> xoe_;

  // The state of this emitter.
  //
  // +-------+   emitNameCallee           +------------+
  // | Start |-+------------------------->| NameCallee |------+
  // +-------+ |                          +------------+      |
  //           |                                              |
  //           | prepareForPropCallee     +------------+      v
  //           +------------------------->| PropCallee |----->+
  //           |                          +------------+      |
  //           |                                              |
  //           | prepareForElemCallee     +------------+      v
  //           +------------------------->| ElemCallee |----->+
  //           |                          +------------+      |
  //           |                                              |
  //           | prepareForPrivateCallee  +---------------+   v
  //           +------------------------->| PrivateCallee |-->+
  //           |                          +---------------+   |
  //           |                                              |
  //           | prepareForFunctionCallee +----------------+  v
  //           +------------------------->| FunctionCallee |->+
  //           |                          +----------------+  |
  //           |                                              |
  //           | emitSuperCallee          +-------------+     v
  //           +------------------------->| SuperCallee |---->+
  //           |                          +-------------+     |
  //           |                                              |
  //           | prepareForOtherCallee    +-------------+     v
  //           +------------------------->| OtherCallee |---->+
  //                                      +-------------+     |
  //                                                          |
  // +--------------------------------------------------------+
  // |
  // | emitThis +------+
  // +--------->| This |-+
  //            +------+ |
  //                     |
  // +-------------------+
  // |
  // | [!isSpread]
  // |   prepareForNonSpreadArguments    +-----------+ emitEnd +-----+
  // +------------------------------->+->| Arguments |-------->| End |
  // |                                ^  +-----------+         +-----+
  // |                                |
  // |                                | wantSpreadIteration
  // |                                |
  // |                                |         +-----------------+
  // |                                +---------| SpreadIteration |------+
  // |                                          +-----------------+      |
  // | [isSpread]                                                        |
  // |   wantSpreadOperand +-------------------+ emitSpreadArgumentsTest |
  // +-------------------->| WantSpreadOperand |-------------------------+
  //                       +-------------------+
  enum class State {
    // The initial state.
    Start,

    // After calling emitNameCallee.
    NameCallee,

    // After calling prepareForPropCallee.
    PropCallee,

    // After calling prepareForElemCallee.
    ElemCallee,

    // After calling prepareForPrivateCallee.
    PrivateCallee,

    // After calling prepareForFunctionCallee.
    FunctionCallee,

    // After calling emitSuperCallee.
    SuperCallee,

    // After calling prepareForOtherCallee.
    OtherCallee,

    // After calling emitThis.
    This,

    // After calling wantSpreadOperand.
    WantSpreadOperand,

    // After calling emitSpreadArgumentsTest.
    SpreadIteration,

    // After calling prepareForNonSpreadArguments.
    Arguments,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;

 public:
  CallOrNewEmitter(BytecodeEmitter* bce, JSOp op, ArgumentsKind argumentsKind,
                   ValueUsage valueUsage);

 private:
  [[nodiscard]] bool isCall() const {
    return op_ == JSOp::Call || op_ == JSOp::CallIgnoresRv ||
           op_ == JSOp::SpreadCall || isEval() || isFunApply() || isFunCall();
  }

  [[nodiscard]] bool isNew() const {
    return op_ == JSOp::New || op_ == JSOp::SpreadNew;
  }

  [[nodiscard]] bool isSuperCall() const {
    return op_ == JSOp::SuperCall || op_ == JSOp::SpreadSuperCall;
  }

  [[nodiscard]] bool isEval() const {
    return op_ == JSOp::Eval || op_ == JSOp::StrictEval ||
           op_ == JSOp::SpreadEval || op_ == JSOp::StrictSpreadEval;
  }

  [[nodiscard]] bool isFunApply() const { return op_ == JSOp::FunApply; }

  [[nodiscard]] bool isFunCall() const { return op_ == JSOp::FunCall; }

  [[nodiscard]] bool isSpread() const { return IsSpreadOp(op_); }

  [[nodiscard]] bool isSingleSpread() const {
    return argumentsKind_ == ArgumentsKind::SingleSpread;
  }

  [[nodiscard]] bool isPassthroughRest() const {
    return argumentsKind_ == ArgumentsKind::PassthroughRest;
  }

 public:
  [[nodiscard]] bool emitNameCallee(TaggedParserAtomIndex name);
  [[nodiscard]] PropOpEmitter& prepareForPropCallee(bool isSuperProp);
  [[nodiscard]] ElemOpEmitter& prepareForElemCallee(bool isSuperElem);
  [[nodiscard]] PrivateOpEmitter& prepareForPrivateCallee(
      TaggedParserAtomIndex privateName);
  [[nodiscard]] bool prepareForFunctionCallee();
  [[nodiscard]] bool emitSuperCallee();
  [[nodiscard]] bool prepareForOtherCallee();

  [[nodiscard]] bool emitThis();

  // Used by BytecodeEmitter::emitPipeline to reuse CallOrNewEmitter instance
  // across multiple chained calls.
  void reset();

  [[nodiscard]] bool prepareForNonSpreadArguments();

  // See the usage in the comment at the top of the class.
  [[nodiscard]] bool wantSpreadOperand();
  [[nodiscard]] bool emitSpreadArgumentsTest();
  [[nodiscard]] bool wantSpreadIteration();

  // Parameters are the offset in the source code for each character below:
  //
  //   callee(arg);
  //   ^
  //   |
  //   beginPos
  //
  // Can be Nothing() if not available.
  [[nodiscard]] bool emitEnd(uint32_t argc,
                             const mozilla::Maybe<uint32_t>& beginPos);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_CallOrNewEmitter_h */
