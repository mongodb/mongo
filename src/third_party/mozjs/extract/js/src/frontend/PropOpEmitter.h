/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_PropOpEmitter_h
#define frontend_PropOpEmitter_h

#include "mozilla/Attributes.h"

#include <stddef.h>

#include "vm/SharedStencil.h"  // GCThingIndex

namespace js {
namespace frontend {

struct BytecodeEmitter;
class TaggedParserAtomIndex;
enum class ValueUsage;

// Class for emitting bytecode for property operation.
//
// Usage: (check for the return value is omitted for simplicity)
//
//   `obj.prop;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Get,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitGet(atom_of_prop);
//
//   `super.prop;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Get,
//                       PropOpEmitter::ObjKind::Super);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitGet(atom_of_prop);
//
//   `obj.prop();`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Call,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitGet(atom_of_prop);
//     emit_call_here();
//
//   `new obj.prop();`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Call,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitGet(atom_of_prop);
//     emit_call_here();
//
//   `delete obj.prop;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Delete,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitDelete(atom_of_prop);
//
//   `delete super.prop;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::Delete,
//                       PropOpEmitter::ObjKind::Other);
//     poe.emitDelete(atom_of_prop);
//
//   `obj.prop++;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::PostIncrement,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitIncDec(atom_of_prop);
//
//   `obj.prop = value;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::SimpleAssignment,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.prepareForRhs();
//     emit(value);
//     poe.emitAssignment(atom_of_prop);
//
//   `obj.prop += value;`
//     PropOpEmitter poe(this,
//                       PropOpEmitter::Kind::CompoundAssignment,
//                       PropOpEmitter::ObjKind::Other);
//     poe.prepareForObj();
//     emit(obj);
//     poe.emitGet(atom_of_prop);
//     poe.prepareForRhs();
//     emit(value);
//     emit_add_op_here();
//     poe.emitAssignment(nullptr); // nullptr for CompoundAssignment
//
class MOZ_STACK_CLASS PropOpEmitter {
 public:
  enum class Kind {
    Get,
    Call,
    Delete,
    PostIncrement,
    PreIncrement,
    PostDecrement,
    PreDecrement,
    SimpleAssignment,
    PropInit,
    CompoundAssignment
  };
  enum class ObjKind { Super, Other };

 private:
  BytecodeEmitter* bce_;

  Kind kind_;
  ObjKind objKind_;

  // The index for the property name's atom.
  GCThingIndex propAtomIndex_;

#ifdef DEBUG
  // The state of this emitter.
  //
  //
  // +-------+   prepareForObj +-----+
  // | Start |---------------->| Obj |-+
  // +-------+                 +-----+ |
  //                                   |
  // +---------------------------------+
  // |
  // |
  // | [Get]
  // | [Call]
  // |   emitGet +-----+
  // +---------->| Get |
  // |           +-----+
  // |
  // | [Delete]
  // |   emitDelete +--------+
  // +------------->| Delete |
  // |              +--------+
  // |
  // | [PostIncrement]
  // | [PreIncrement]
  // | [PostDecrement]
  // | [PreDecrement]
  // |   emitIncDec +--------+
  // +------------->| IncDec |
  // |              +--------+
  // |
  // | [SimpleAssignment]
  // | [PropInit]
  // |                        prepareForRhs    +-----+
  // +--------------------->+----------------->| Rhs |-+
  // |                      ^                  +-----+ |
  // |                      |                          |
  // |                      |                +---------+
  // | [CompoundAssignment] |                |
  // |   emitGet +-----+    |                | emitAssignment +------------+
  // +---------->| Get |----+                + -------------->| Assignment |
  //             +-----+                                      +------------+
  enum class State {
    // The initial state.
    Start,

    // After calling prepareForObj.
    Obj,

    // After calling emitGet.
    Get,

    // After calling emitDelete.
    Delete,

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
  PropOpEmitter(BytecodeEmitter* bce, Kind kind, ObjKind objKind);

 private:
  [[nodiscard]] bool isCall() const { return kind_ == Kind::Call; }

  [[nodiscard]] bool isSuper() const { return objKind_ == ObjKind::Super; }

  [[nodiscard]] bool isSimpleAssignment() const {
    return kind_ == Kind::SimpleAssignment;
  }

  [[nodiscard]] bool isPropInit() const { return kind_ == Kind::PropInit; }

  [[nodiscard]] bool isDelete() const { return kind_ == Kind::Delete; }

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

  [[nodiscard]] bool prepareAtomIndex(TaggedParserAtomIndex prop);

 public:
  [[nodiscard]] bool prepareForObj();

  [[nodiscard]] bool emitGet(TaggedParserAtomIndex prop);

  [[nodiscard]] bool prepareForRhs();

  [[nodiscard]] bool emitDelete(TaggedParserAtomIndex prop);

  // `prop` can be nullptr for CompoundAssignment.
  [[nodiscard]] bool emitAssignment(TaggedParserAtomIndex prop);

  [[nodiscard]] bool emitIncDec(TaggedParserAtomIndex prop,
                                ValueUsage valueUsage);

  size_t numReferenceSlots() const { return 1 + isSuper(); }
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_PropOpEmitter_h */
