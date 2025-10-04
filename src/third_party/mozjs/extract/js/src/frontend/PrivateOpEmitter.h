/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_PrivateOpEmitter_h
#define frontend_PrivateOpEmitter_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stddef.h>

#include "frontend/NameAnalysisTypes.h"  // NameLocation
#include "frontend/ParserAtom.h"         // TaggedParserAtomIndex

namespace js {
namespace frontend {

struct BytecodeEmitter;
enum class ValueUsage;

// Class for emitting bytecode for operations on private members of objects.
//
// Usage is similar to PropOpEmitter, but the name of the private member must
// be passed to the constructor; prepare*() methods aren't necessary; and
// `delete obj.#member` and `super.#member` aren't supported because both are
// SyntaxErrors.
//
// Usage: (error checking is omitted for simplicity)
//
//   `obj.#member;`
//     PrivateOpEmitter xoe(this,
//                          privateName,
//                          PrivateOpEmitter::Kind::Get);
//     emit(obj);
//     xoe.emitReference();
//     xoe.emitGet();
//
//   `obj.#member();`
//     PrivateOpEmitter xoe(this,
//                          privateName,
//                          PrivateOpEmitter::Kind::Call);
//     emit(obj);
//     xoe.emitReference();
//     xoe.emitGet();
//     emit_call_here();
//
//   `new obj.#member();`
//     The same, but use PrivateOpEmitter::Kind::Get.
//
//   `obj.#field++;`
//     PrivateOpEmitter xoe(this,
//                          privateName,
//                          PrivateOpEmitter::Kind::PostIncrement);
//     emit(obj);
//     xoe.emitReference();
//     xoe.emitIncDec();
//
//   `obj.#field = value;`
//     PrivateOpEmitter xoe(this,
//                          privateName,
//                          PrivateOpEmitter::Kind::SimpleAssignment);
//     emit(obj);
//     xoe.emitReference();
//     emit(value);
//     xoe.emitAssignment();
//
//   `obj.#field += value;`
//     PrivateOpEmitter xoe(this,
//                          privateName,
//                          PrivateOpEmitter::Kind::CompoundAssignment);
//     emit(obj);
//     xoe.emitReference();
//     emit(JSOp::Dup2);
//     xoe.emitGet();
//     emit(value);
//     emit_add_op_here();
//     xoe.emitAssignment();
//
class MOZ_STACK_CLASS PrivateOpEmitter {
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
    CompoundAssignment,
    ErgonomicBrandCheck,
  };

 private:
  BytecodeEmitter* bce_;

  Kind kind_;

  // Name of the private member, e.g. "#field".
  TaggedParserAtomIndex name_;

  // Location of the slot containing the private name symbol; or, for a
  // non-static private method, the slot containing the method.
  mozilla::Maybe<NameLocation> loc_;

  // For non-static private method accesses, the location of the relevant
  // `.privateBrand` binding. Otherwise, `Nothing`.
  mozilla::Maybe<NameLocation> brandLoc_{};

#ifdef DEBUG
  // The state of this emitter.
  //
  //            emitReference
  // +-------+  skipReference  +-----------+
  // | Start |---------------->| Reference |
  // +-------+                 +-----+-----+
  //                                 |
  //     +---------------------------+
  //     |
  //     |
  //     | [Get]
  //     |   emitGet
  //     | [Call] [Get]                  [CompoundAssignment]
  //     |   emitGetForCallOrNew +-----+   emitAssignment
  //     +---------------------->| Get |-------------------+
  //     |                       +-----+                   |
  //     | [PostIncrement]                                 |
  //     | [PreIncrement]                                  |
  //     | [PostDecrement]                                 |
  //     | [PreDecrement]                                  |
  //     |   emitIncDec                                    |
  //     +------------------------------------------------>+
  //     |                                                 |
  //     | [SimpleAssignment]                              |
  //     | [PropInit]                                      V
  //     |   emitAssignment                          +------------+
  //     +------------------------------------------>| Assignment |
  //                                                 +------------+
  enum class State {
    // The initial state.
    Start,

    // After calling emitReference or skipReference.
    Reference,

    // After calling emitGet.
    Get,

    // After calling emitAssignment or emitIncDec.
    Assignment,
  };
  State state_ = State::Start;
#endif

 public:
  PrivateOpEmitter(BytecodeEmitter* bce, Kind kind, TaggedParserAtomIndex name);

 private:
  [[nodiscard]] bool isCall() const { return kind_ == Kind::Call; }

  [[nodiscard]] bool isSimpleAssignment() const {
    return kind_ == Kind::SimpleAssignment;
  }

  [[nodiscard]] bool isFieldInit() const { return kind_ == Kind::PropInit; }

  [[nodiscard]] bool isBrandCheck() const {
    return kind_ == Kind::ErgonomicBrandCheck;
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

  [[nodiscard]] bool init();

  // Emit a GetAliasedLexical or similar instruction.
  [[nodiscard]] bool emitLoad(TaggedParserAtomIndex name,
                              const NameLocation& loc);

  [[nodiscard]] bool emitLoadPrivateBrand();

 public:
  // Emit bytecode to check for the presence/absence of a private field/brand.
  //
  // Given OBJ KEY on the stack, where KEY is a private name symbol, the
  // emitted code will throw if OBJ does not have the given KEY.
  //
  // If `isFieldInit()`, the check is reversed: the code will throw if OBJ
  // already has the KEY.
  //
  // If `isBrandCheck()`, the check verifies RHS is an object (throwing if not).
  //
  // The bytecode leaves OBJ KEY BOOL on the stack. Caller is responsible for
  // consuming or popping it.
  [[nodiscard]] bool emitBrandCheck();

  [[nodiscard]] bool emitReference();
  [[nodiscard]] bool skipReference();
  [[nodiscard]] bool emitGet();
  [[nodiscard]] bool emitGetForCallOrNew();
  [[nodiscard]] bool emitAssignment();
  [[nodiscard]] bool emitIncDec(ValueUsage valueUsage);

  [[nodiscard]] size_t numReferenceSlots() { return 2; }
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_PrivateOpEmitter_h */
