/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameAnalysisTypes_h
#define frontend_NameAnalysisTypes_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/Casting.h"     // mozilla::AssertedCast

#include <stdint.h>  // uint8_t, uint16_t, uint32_t

#include "vm/BindingKind.h"          // BindingKind, BindingLocation
#include "vm/BytecodeFormatFlags.h"  // JOF_ENVCOORD
#include "vm/BytecodeUtil.h"  // ENVCOORD_HOPS_BITS, ENVCOORD_SLOT_BITS, GET_ENVCOORD_HOPS, GET_ENVCOORD_SLOT, ENVCOORD_HOPS_LEN, JOF_OPTYPE, JSOp, LOCALNO_LIMIT

namespace js {

// An "environment coordinate" describes how to get from head of the
// environment chain to a given lexically-enclosing variable. An environment
// coordinate has two dimensions:
//  - hops: the number of environment objects on the scope chain to skip
//  - slot: the slot on the environment object holding the variable's value
class EnvironmentCoordinate {
  uint32_t hops_;
  uint32_t slot_;

  // Technically, hops_/slot_ are ENVCOORD_(HOPS|SLOT)_BITS wide.  Since
  // EnvironmentCoordinate is a temporary value, don't bother with a bitfield as
  // this only adds overhead.
  static_assert(ENVCOORD_HOPS_BITS <= 32, "We have enough bits below");
  static_assert(ENVCOORD_SLOT_BITS <= 32, "We have enough bits below");

 public:
  explicit inline EnvironmentCoordinate(jsbytecode* pc)
      : hops_(GET_ENVCOORD_HOPS(pc)),
        slot_(GET_ENVCOORD_SLOT(pc + ENVCOORD_HOPS_LEN)) {
    MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_ENVCOORD ||
               JOF_OPTYPE(JSOp(*pc)) == JOF_DEBUGCOORD);
  }

  EnvironmentCoordinate() = default;

  void setHops(uint32_t hops) {
    MOZ_ASSERT(hops < ENVCOORD_HOPS_LIMIT);
    hops_ = hops;
  }

  void setSlot(uint32_t slot) {
    MOZ_ASSERT(slot < ENVCOORD_SLOT_LIMIT);
    slot_ = slot;
  }

  uint32_t hops() const {
    MOZ_ASSERT(hops_ < ENVCOORD_HOPS_LIMIT);
    return hops_;
  }

  uint32_t slot() const {
    MOZ_ASSERT(slot_ < ENVCOORD_SLOT_LIMIT);
    return slot_;
  }

  bool operator==(const EnvironmentCoordinate& rhs) const {
    return hops() == rhs.hops() && slot() == rhs.slot();
  }
};

namespace frontend {

enum class ParseGoal : uint8_t { Script, Module };

// A detailed kind used for tracking declarations in the Parser. Used for
// specific early error semantics and better error messages.
enum class DeclarationKind : uint8_t {
  PositionalFormalParameter,
  FormalParameter,
  CoverArrowParameter,
  Var,
  Let,
  Const,
  Class,  // Handled as same as `let` after parsing.
  Import,
  BodyLevelFunction,
  ModuleBodyLevelFunction,
  LexicalFunction,
  SloppyLexicalFunction,
  VarForAnnexBLexicalFunction,
  SimpleCatchParameter,
  CatchParameter,
  PrivateName,
  Synthetic,
  PrivateMethod,  // slot to store nonstatic private method
};

// Class field kind.
enum class FieldPlacement : uint8_t { Unspecified, Instance, Static };

static inline BindingKind DeclarationKindToBindingKind(DeclarationKind kind) {
  switch (kind) {
    case DeclarationKind::PositionalFormalParameter:
    case DeclarationKind::FormalParameter:
    case DeclarationKind::CoverArrowParameter:
      return BindingKind::FormalParameter;

    case DeclarationKind::Var:
    case DeclarationKind::BodyLevelFunction:
    case DeclarationKind::ModuleBodyLevelFunction:
    case DeclarationKind::VarForAnnexBLexicalFunction:
      return BindingKind::Var;

    case DeclarationKind::Let:
    case DeclarationKind::Class:
    case DeclarationKind::LexicalFunction:
    case DeclarationKind::SloppyLexicalFunction:
    case DeclarationKind::SimpleCatchParameter:
    case DeclarationKind::CatchParameter:
      return BindingKind::Let;

    case DeclarationKind::Const:
      return BindingKind::Const;

    case DeclarationKind::Import:
      return BindingKind::Import;

    case DeclarationKind::Synthetic:
    case DeclarationKind::PrivateName:
      return BindingKind::Synthetic;

    case DeclarationKind::PrivateMethod:
      return BindingKind::PrivateMethod;
  }

  MOZ_CRASH("Bad DeclarationKind");
}

static inline bool DeclarationKindIsLexical(DeclarationKind kind) {
  return BindingKindIsLexical(DeclarationKindToBindingKind(kind));
}

// Used in Parser and BytecodeEmitter to track the kind of a private name.
enum class PrivateNameKind : uint8_t {
  None,
  Field,
  Method,
  Getter,
  Setter,
  GetterSetter,
};

enum class ClosedOver : bool { No = false, Yes = true };

// Used in Parser to track declared names.
class DeclaredNameInfo {
  uint32_t pos_;
  DeclarationKind kind_;

  // If the declared name is a binding, whether the binding is closed
  // over. Its value is meaningless if the declared name is not a binding
  // (i.e., a 'var' declared name in a non-var scope).
  bool closedOver_;

  PrivateNameKind privateNameKind_;

  // Only updated for private names (see noteDeclaredPrivateName),
  // tracks if declaration was instance or static to allow issuing
  // early errors in the case where we mismatch instance and static
  // private getter/setters.
  FieldPlacement placement_;

 public:
  explicit DeclaredNameInfo(DeclarationKind kind, uint32_t pos,
                            ClosedOver closedOver = ClosedOver::No)
      : pos_(pos),
        kind_(kind),
        closedOver_(bool(closedOver)),
        privateNameKind_(PrivateNameKind::None),
        placement_(FieldPlacement::Unspecified) {}

  // Needed for InlineMap.
  DeclaredNameInfo() = default;

  DeclarationKind kind() const { return kind_; }

  static const uint32_t npos = uint32_t(-1);

  uint32_t pos() const { return pos_; }

  void alterKind(DeclarationKind kind) { kind_ = kind; }

  void setClosedOver() { closedOver_ = true; }

  bool closedOver() const { return closedOver_; }

  void setPrivateNameKind(PrivateNameKind privateNameKind) {
    privateNameKind_ = privateNameKind;
  }

  void setFieldPlacement(FieldPlacement placement) {
    MOZ_ASSERT(placement != FieldPlacement::Unspecified);
    placement_ = placement;
  }

  PrivateNameKind privateNameKind() const { return privateNameKind_; }

  FieldPlacement placement() const { return placement_; }
};

// Used in BytecodeEmitter to map names to locations.
class NameLocation {
 public:
  enum class Kind : uint8_t {
    // Cannot statically determine where the name lives. Needs to walk the
    // environment chain to search for the name.
    Dynamic,

    // The name lives on the global or is a global lexical binding. Search
    // for the name on the global scope.
    Global,

    // Special mode used only when emitting self-hosted scripts. See
    // BytecodeEmitter::lookupName.
    Intrinsic,

    // In a named lambda, the name is the callee itself.
    NamedLambdaCallee,

    // The name is a positional formal parameter name and can be retrieved
    // directly from the stack using slot_.
    ArgumentSlot,

    // The name is not closed over and lives on the frame in slot_.
    FrameSlot,

    // The name is closed over and lives on an environment hops_ away in slot_.
    EnvironmentCoordinate,

    // The name is closed over and lives on an environment hops_ away in slot_,
    // where one or more of the environments may be a DebugEnvironmentProxy
    DebugEnvironmentCoordinate,

    // An imported name in a module.
    Import,

    // Cannot statically determine where the synthesized var for Annex
    // B.3.3 lives.
    DynamicAnnexBVar
  };

 private:
  // Where the name lives.
  Kind kind_;

  // If the name is not Dynamic or DynamicAnnexBVar, the kind of the
  // binding.
  BindingKind bindingKind_;

  // If the name is closed over and accessed via EnvironmentCoordinate, the
  // number of dynamic environments to skip.
  //
  // Otherwise UINT8_MAX.
  uint8_t hops_;

  // If the name lives on the frame, the slot frame.
  //
  // If the name is closed over and accessed via EnvironmentCoordinate, the
  // slot on the environment.
  //
  // Otherwise 0.
  uint32_t slot_ : ENVCOORD_SLOT_BITS;

  static_assert(LOCALNO_BITS == ENVCOORD_SLOT_BITS,
                "Frame and environment slots must be same sized.");

  NameLocation(Kind kind, BindingKind bindingKind, uint8_t hops = UINT8_MAX,
               uint32_t slot = 0)
      : kind_(kind), bindingKind_(bindingKind), hops_(hops), slot_(slot) {}

 public:
  // Default constructor for InlineMap.
  NameLocation() = default;

  static NameLocation Dynamic() {
    return NameLocation(Kind::Dynamic, BindingKind::Import);
  }

  static NameLocation Global(BindingKind bindKind) {
    MOZ_ASSERT(bindKind != BindingKind::FormalParameter);
    return NameLocation(Kind::Global, bindKind);
  }

  static NameLocation Intrinsic() {
    return NameLocation(Kind::Intrinsic, BindingKind::Var);
  }

  static NameLocation NamedLambdaCallee() {
    return NameLocation(Kind::NamedLambdaCallee,
                        BindingKind::NamedLambdaCallee);
  }

  static NameLocation ArgumentSlot(uint16_t slot) {
    return NameLocation(Kind::ArgumentSlot, BindingKind::FormalParameter, 0,
                        slot);
  }

  static NameLocation FrameSlot(BindingKind bindKind, uint32_t slot) {
    MOZ_ASSERT(slot < LOCALNO_LIMIT);
    return NameLocation(Kind::FrameSlot, bindKind, 0, slot);
  }

  static NameLocation EnvironmentCoordinate(BindingKind bindKind, uint8_t hops,
                                            uint32_t slot) {
    MOZ_ASSERT(slot < ENVCOORD_SLOT_LIMIT);
    return NameLocation(Kind::EnvironmentCoordinate, bindKind, hops, slot);
  }
  static NameLocation DebugEnvironmentCoordinate(BindingKind bindKind,
                                                 uint8_t hops, uint32_t slot) {
    MOZ_ASSERT(slot < ENVCOORD_SLOT_LIMIT);
    return NameLocation(Kind::DebugEnvironmentCoordinate, bindKind, hops, slot);
  }

  static NameLocation Import() {
    return NameLocation(Kind::Import, BindingKind::Import);
  }

  static NameLocation DynamicAnnexBVar() {
    return NameLocation(Kind::DynamicAnnexBVar, BindingKind::Var);
  }

  bool operator==(const NameLocation& other) const {
    return kind_ == other.kind_ && bindingKind_ == other.bindingKind_ &&
           hops_ == other.hops_ && slot_ == other.slot_;
  }

  bool operator!=(const NameLocation& other) const { return !(*this == other); }

  Kind kind() const { return kind_; }

  uint16_t argumentSlot() const {
    MOZ_ASSERT(kind_ == Kind::ArgumentSlot);
    return mozilla::AssertedCast<uint16_t>(slot_);
  }

  uint32_t frameSlot() const {
    MOZ_ASSERT(kind_ == Kind::FrameSlot);
    return slot_;
  }

  NameLocation addHops(uint8_t more) {
    MOZ_ASSERT(hops_ < ENVCOORD_HOPS_LIMIT - more);
    MOZ_ASSERT(kind_ == Kind::EnvironmentCoordinate);
    return NameLocation(kind_, bindingKind_, hops_ + more, slot_);
  }

  class EnvironmentCoordinate environmentCoordinate() const {
    MOZ_ASSERT(kind_ == Kind::EnvironmentCoordinate ||
               kind_ == Kind::DebugEnvironmentCoordinate);
    class EnvironmentCoordinate coord;
    coord.setHops(hops_);
    coord.setSlot(slot_);
    return coord;
  }

  BindingKind bindingKind() const {
    MOZ_ASSERT(kind_ != Kind::Dynamic);
    return bindingKind_;
  }

  bool isLexical() const { return BindingKindIsLexical(bindingKind()); }

  bool isConst() const { return bindingKind() == BindingKind::Const; }

  bool isSynthetic() const { return bindingKind() == BindingKind::Synthetic; }

  bool isPrivateMethod() const {
    return bindingKind() == BindingKind::PrivateMethod;
  }

  bool hasKnownSlot() const {
    return kind_ == Kind::ArgumentSlot || kind_ == Kind::FrameSlot ||
           kind_ == Kind::EnvironmentCoordinate;
  }
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_NameAnalysisTypes_h
