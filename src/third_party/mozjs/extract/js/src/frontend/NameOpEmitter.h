/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameOpEmitter_h
#define frontend_NameOpEmitter_h

#include "mozilla/Attributes.h"

#include "frontend/NameAnalysisTypes.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "vm/SharedStencil.h"     // GCThingIndex

namespace js {
namespace frontend {

struct BytecodeEmitter;
enum class ValueUsage;

// Class for emitting bytecode for name operation.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `name;`
//     NameOpEmitter noe(this, atom_of_name
//                       NameOpEmitter::Kind::Get);
//     noe.emitGet();
//
//   `name();`
//     this is handled in CallOrNewEmitter
//
//   `name++;`
//     NameOpEmitter noe(this, atom_of_name
//                       NameOpEmitter::Kind::PostIncrement);
//     noe.emitIncDec();
//
//   `name = 10;`
//     NameOpEmitter noe(this, atom_of_name
//                       NameOpEmitter::Kind::SimpleAssignment);
//     noe.prepareForRhs();
//     emit(10);
//     noe.emitAssignment();
//
//   `name += 10;`
//     NameOpEmitter noe(this, atom_of_name
//                       NameOpEmitter::Kind::CompoundAssignment);
//     noe.prepareForRhs();
//     emit(10);
//     emit_add_op_here();
//     noe.emitAssignment();
//
//   `name = 10;` part of `let name = 10;`
//     NameOpEmitter noe(this, atom_of_name
//                       NameOpEmitter::Kind::Initialize);
//     noe.prepareForRhs();
//     emit(10);
//     noe.emitAssignment();
//
class MOZ_STACK_CLASS NameOpEmitter {
 public:
  enum class Kind {
    Get,
    Call,
    PostIncrement,
    PreIncrement,
    PostDecrement,
    PreDecrement,
    SimpleAssignment,
    CompoundAssignment,
    Initialize
  };

 private:
  BytecodeEmitter* bce_;

  Kind kind_;

  bool emittedBindOp_ = false;

  TaggedParserAtomIndex name_;

  GCThingIndex atomIndex_;

  NameLocation loc_;

#ifdef DEBUG
  // The state of this emitter.
  //
  //               [Get]
  //               [Call]
  // +-------+      emitGet +-----+
  // | Start |-+-+--------->| Get |
  // +-------+   |          +-----+
  //             |
  //             | [PostIncrement]
  //             | [PreIncrement]
  //             | [PostDecrement]
  //             | [PreDecrement]
  //             |   emitIncDec +--------+
  //             +------------->| IncDec |
  //             |              +--------+
  //             |
  //             | [SimpleAssignment]
  //             |                        prepareForRhs +-----+
  //             +--------------------->+-------------->| Rhs |-+
  //             |                      ^               +-----+ |
  //             |                      |                       |
  //             |                      |    +------------------+
  //             | [CompoundAssignment] |    |
  //             |   emitGet +-----+    |    | emitAssignment +------------+
  //             +---------->| Get |----+    + -------------->| Assignment |
  //                         +-----+                          +------------+
  enum class State {
    // The initial state.
    Start,

    // After calling emitGet.
    Get,

    // After calling emitIncDec.
    IncDec,

    // After calling prepareForRhs.
    Rhs,

    // After calling emitAssignment.
    Assignment,
  };
  State state_ = State::Start;
#endif

 public:
  NameOpEmitter(BytecodeEmitter* bce, TaggedParserAtomIndex name, Kind kind);
  NameOpEmitter(BytecodeEmitter* bce, TaggedParserAtomIndex name,
                const NameLocation& loc, Kind kind);

 private:
  [[nodiscard]] bool isCall() const { return kind_ == Kind::Call; }

  [[nodiscard]] bool isSimpleAssignment() const {
    return kind_ == Kind::SimpleAssignment;
  }

  [[nodiscard]] bool isCompoundAssignment() const {
    return kind_ == Kind::CompoundAssignment;
  }

  [[nodiscard]] bool isIncDec() const {
    return isPostIncDec() || isPreIncDec();
  }

  [[nodiscard]] bool isPostIncDec() const {
    return kind_ == Kind::PostIncrement || kind_ == Kind::PostDecrement;
  }

  [[nodiscard]] bool isPreIncDec() const {
    return kind_ == Kind::PreIncrement || kind_ == Kind::PreDecrement;
  }

  [[nodiscard]] bool isInc() const {
    return kind_ == Kind::PostIncrement || kind_ == Kind::PreIncrement;
  }

  [[nodiscard]] bool isInitialize() const { return kind_ == Kind::Initialize; }

 public:
  [[nodiscard]] bool emittedBindOp() const { return emittedBindOp_; }

  [[nodiscard]] const NameLocation& loc() const { return loc_; }

  [[nodiscard]] bool emitGet();
  [[nodiscard]] bool prepareForRhs();
  [[nodiscard]] bool emitAssignment();
  [[nodiscard]] bool emitIncDec(ValueUsage valueUsage);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_NameOpEmitter_h */
