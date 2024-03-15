/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ExpressionStatementEmitter_h
#define frontend_ExpressionStatementEmitter_h

#include "mozilla/Attributes.h"

#include <stdint.h>

#include "frontend/ValueUsage.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;

// Class for emitting bytecode for expression statement.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `expr;`
//     // IgnoreValue if this is in normal script.
//     // WantValue if this is in eval script.
//     ValueUsage valueUsage = ...;
//
//     ExpressionStatementEmitter ese(this, valueUsage);
//     ese.prepareForExpr(offset_of_expr);
//     emit(expr);
//     ese.emitEnd();
//
class MOZ_STACK_CLASS ExpressionStatementEmitter {
  BytecodeEmitter* bce_;

#ifdef DEBUG
  // The stack depth before emitting expression.
  int32_t depth_;
#endif

  // The usage of the value of the expression.
  ValueUsage valueUsage_;

#ifdef DEBUG
  // The state of this emitter.
  //
  // +-------+ prepareForExpr +------+ emitEnd +-----+
  // | Start |--------------->| Expr |-------->| End |
  // +-------+                +------+         +-----+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForExpr.
    Expr,

    // After calling emitEnd.
    End
  };
  State state_ = State::Start;
#endif

 public:
  ExpressionStatementEmitter(BytecodeEmitter* bce, ValueUsage valueUsage);

  // Parameters are the offset in the source code for each character below:
  //
  //   expr;
  //   ^
  //   |
  //   beginPos
  [[nodiscard]] bool prepareForExpr(uint32_t beginPos);
  [[nodiscard]] bool emitEnd();
};

}  // namespace frontend
}  // namespace js

#endif /* frontend_ExpressionStatementEmitter_h */
