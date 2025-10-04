/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BytecodeUtil_inl_h
#define vm_BytecodeUtil_inl_h

#include "vm/BytecodeUtil.h"

#include "frontend/SourceNotes.h"  // SrcNote, SrcNoteType, SrcNoteIterator
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::ColumnNumberOffset
#include "vm/JSScript.h"

namespace js {

static inline unsigned GetDefCount(jsbytecode* pc) {
  /*
   * Add an extra pushed value for Or/And opcodes, so that they are included
   * in the pushed array of stack values for type inference.
   */
  JSOp op = JSOp(*pc);
  switch (op) {
    case JSOp::Or:
    case JSOp::And:
    case JSOp::Coalesce:
      return 1;
    case JSOp::Pick:
    case JSOp::Unpick:
      /*
       * Pick pops and pushes how deep it looks in the stack + 1
       * items. i.e. if the stack were |a b[2] c[1] d[0]|, pick 2
       * would pop b, c, and d to rearrange the stack to |a c[0]
       * d[1] b[2]|.
       */
      return pc[1] + 1;
    default:
      return StackDefs(op);
  }
}

static inline unsigned GetUseCount(jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  if (op == JSOp::Pick || op == JSOp::Unpick) {
    return pc[1] + 1;
  }

  return StackUses(op, pc);
}

static inline JSOp ReverseCompareOp(JSOp op) {
  switch (op) {
    case JSOp::Gt:
      return JSOp::Lt;
    case JSOp::Ge:
      return JSOp::Le;
    case JSOp::Lt:
      return JSOp::Gt;
    case JSOp::Le:
      return JSOp::Ge;
    case JSOp::Eq:
    case JSOp::Ne:
    case JSOp::StrictEq:
    case JSOp::StrictNe:
      return op;
    default:
      MOZ_CRASH("unrecognized op");
  }
}

static inline JSOp NegateCompareOp(JSOp op) {
  switch (op) {
    case JSOp::Gt:
      return JSOp::Le;
    case JSOp::Ge:
      return JSOp::Lt;
    case JSOp::Lt:
      return JSOp::Ge;
    case JSOp::Le:
      return JSOp::Gt;
    case JSOp::Eq:
      return JSOp::Ne;
    case JSOp::Ne:
      return JSOp::Eq;
    case JSOp::StrictNe:
      return JSOp::StrictEq;
    case JSOp::StrictEq:
      return JSOp::StrictNe;
    default:
      MOZ_CRASH("unrecognized op");
  }
}

class BytecodeRange {
 public:
  BytecodeRange(JSContext* cx, JSScript* script)
      : script(cx, script), pc(script->code()), end(pc + script->length()) {}
  bool empty() const { return pc == end; }
  jsbytecode* frontPC() const { return pc; }
  JSOp frontOpcode() const { return JSOp(*pc); }
  size_t frontOffset() const { return script->pcToOffset(pc); }
  void popFront() { pc += GetBytecodeLength(pc); }

 private:
  RootedScript script;
  jsbytecode* pc;
  jsbytecode* end;
};

class BytecodeRangeWithPosition : private BytecodeRange {
 public:
  using BytecodeRange::empty;
  using BytecodeRange::frontOffset;
  using BytecodeRange::frontOpcode;
  using BytecodeRange::frontPC;

  BytecodeRangeWithPosition(JSContext* cx, JSScript* script)
      : BytecodeRange(cx, script),
        initialLine(script->lineno()),
        lineno(script->lineno()),
        column(script->column()),
        sn(script->notes()),
        snEnd(script->notesEnd()),
        snpc(script->code()),
        isEntryPoint(false),
        isBreakpoint(false),
        seenStepSeparator(false),
        wasArtifactEntryPoint(false) {
    if (sn < snEnd) {
      snpc += sn->delta();
    }
    updatePosition();
    while (frontPC() != script->main()) {
      popFront();
    }

    if (frontOpcode() != JSOp::JumpTarget) {
      isEntryPoint = true;
    } else {
      wasArtifactEntryPoint = true;
    }
  }

  void popFront() {
    BytecodeRange::popFront();
    if (empty()) {
      isEntryPoint = false;
    } else {
      updatePosition();
    }

    // The following conditions are handling artifacts introduced by the
    // bytecode emitter, such that we do not add breakpoints on empty
    // statements of the source code of the user.
    if (wasArtifactEntryPoint) {
      wasArtifactEntryPoint = false;
      isEntryPoint = true;
    }

    if (isEntryPoint && frontOpcode() == JSOp::JumpTarget) {
      wasArtifactEntryPoint = isEntryPoint;
      isEntryPoint = false;
    }
  }

  uint32_t frontLineNumber() const { return lineno; }
  JS::LimitedColumnNumberOneOrigin frontColumnNumber() const { return column; }

  // Entry points are restricted to bytecode offsets that have an
  // explicit mention in the line table.  This restriction avoids a
  // number of failing cases caused by some instructions not having
  // sensible (to the user) line numbers, and it is one way to
  // implement the idea that the bytecode emitter should tell the
  // debugger exactly which offsets represent "interesting" (to the
  // user) places to stop.
  bool frontIsEntryPoint() const { return isEntryPoint; }

  // Breakable points are explicitly marked by the emitter as locations where
  // the debugger may want to allow users to pause.
  bool frontIsBreakablePoint() const { return isBreakpoint; }

  // Breakable step points are the first breakable point after a
  // SrcNote::StepSep note has been encountered.
  bool frontIsBreakableStepPoint() const {
    return isBreakpoint && seenStepSeparator;
  }

 private:
  void updatePosition() {
    if (isBreakpoint) {
      isBreakpoint = false;
      seenStepSeparator = false;
    }

    // Determine the current line number by reading all source notes up to
    // and including the current offset.
    jsbytecode* lastLinePC = nullptr;
    SrcNoteIterator iter(sn, snEnd);
    while (!iter.atEnd() && snpc <= frontPC()) {
      auto sn = *iter;

      SrcNoteType type = sn->type();
      if (type == SrcNoteType::ColSpan) {
        column += SrcNote::ColSpan::getSpan(sn);
      } else if (type == SrcNoteType::SetLine) {
        lineno = SrcNote::SetLine::getLine(sn, initialLine);
        column = JS::LimitedColumnNumberOneOrigin();
      } else if (type == SrcNoteType::SetLineColumn) {
        lineno = SrcNote::SetLineColumn::getLine(sn, initialLine);
        column = SrcNote::SetLineColumn::getColumn(sn);
      } else if (type == SrcNoteType::NewLine) {
        lineno++;
        column = JS::LimitedColumnNumberOneOrigin();
      } else if (type == SrcNoteType::NewLineColumn) {
        lineno++;
        column = SrcNote::NewLineColumn::getColumn(sn);
      } else if (type == SrcNoteType::Breakpoint) {
        isBreakpoint = true;
      } else if (type == SrcNoteType::BreakpointStepSep) {
        isBreakpoint = true;
        seenStepSeparator = true;
      }
      lastLinePC = snpc;
      ++iter;
      if (!iter.atEnd()) {
        snpc += (*iter)->delta();
      }
    }

    sn = *iter;
    isEntryPoint = lastLinePC == frontPC();
  }

  uint32_t initialLine;

  // Line number (1-origin).
  uint32_t lineno;

  // Column number in UTF-16 code units.
  JS::LimitedColumnNumberOneOrigin column;

  const SrcNote* sn;
  const SrcNote* snEnd;
  jsbytecode* snpc;
  bool isEntryPoint;
  bool isBreakpoint;
  bool seenStepSeparator;
  bool wasArtifactEntryPoint;
};

}  // namespace js

#endif /* vm_BytecodeUtil_inl_h */
