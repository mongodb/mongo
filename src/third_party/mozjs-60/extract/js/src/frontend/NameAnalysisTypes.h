/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameAnalysisTypes_h
#define frontend_NameAnalysisTypes_h

#include "vm/BytecodeUtil.h"
#include "vm/Scope.h"

namespace js {

// An "environment coordinate" describes how to get from head of the
// environment chain to a given lexically-enclosing variable. An environment
// coordinate has two dimensions:
//  - hops: the number of environment objects on the scope chain to skip
//  - slot: the slot on the environment object holding the variable's value
class EnvironmentCoordinate
{
    uint32_t hops_;
    uint32_t slot_;

    // Technically, hops_/slot_ are ENVCOORD_(HOPS|SLOT)_BITS wide.  Since
    // EnvironmentCoordinate is a temporary value, don't bother with a bitfield as
    // this only adds overhead.
    static_assert(ENVCOORD_HOPS_BITS <= 32, "We have enough bits below");
    static_assert(ENVCOORD_SLOT_BITS <= 32, "We have enough bits below");

  public:
    explicit inline EnvironmentCoordinate(jsbytecode* pc)
      : hops_(GET_ENVCOORD_HOPS(pc)), slot_(GET_ENVCOORD_SLOT(pc + ENVCOORD_HOPS_LEN))
    {
        MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_ENVCOORD);
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

// A detailed kind used for tracking declarations in the Parser. Used for
// specific early error semantics and better error messages.
enum class DeclarationKind : uint8_t
{
    PositionalFormalParameter,
    FormalParameter,
    CoverArrowParameter,
    Var,
    ForOfVar,
    Let,
    Const,
    Class, // Handled as same as `let` after parsing.
    Import,
    BodyLevelFunction,
    ModuleBodyLevelFunction,
    LexicalFunction,
    SloppyLexicalFunction,
    VarForAnnexBLexicalFunction,
    SimpleCatchParameter,
    CatchParameter
};

static inline BindingKind
DeclarationKindToBindingKind(DeclarationKind kind)
{
    switch (kind) {
      case DeclarationKind::PositionalFormalParameter:
      case DeclarationKind::FormalParameter:
      case DeclarationKind::CoverArrowParameter:
        return BindingKind::FormalParameter;

      case DeclarationKind::Var:
      case DeclarationKind::BodyLevelFunction:
      case DeclarationKind::ModuleBodyLevelFunction:
      case DeclarationKind::VarForAnnexBLexicalFunction:
      case DeclarationKind::ForOfVar:
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
    }

    MOZ_CRASH("Bad DeclarationKind");
}

static inline bool
DeclarationKindIsLexical(DeclarationKind kind)
{
    return BindingKindIsLexical(DeclarationKindToBindingKind(kind));
}

// Used in Parser to track declared names.
class DeclaredNameInfo
{
    uint32_t pos_;
    DeclarationKind kind_;

    // If the declared name is a binding, whether the binding is closed
    // over. Its value is meaningless if the declared name is not a binding
    // (i.e., a 'var' declared name in a non-var scope).
    bool closedOver_;

  public:
    explicit DeclaredNameInfo(DeclarationKind kind, uint32_t pos)
      : pos_(pos),
        kind_(kind),
        closedOver_(false)
    { }

    // Needed for InlineMap.
    DeclaredNameInfo() = default;

    DeclarationKind kind() const {
        return kind_;
    }

    static const uint32_t npos = uint32_t(-1);

    uint32_t pos() const {
        return pos_;
    }

    void alterKind(DeclarationKind kind) {
        kind_ = kind;
    }

    void setClosedOver() {
        closedOver_ = true;
    }

    bool closedOver() const {
        return closedOver_;
    }
};

// Used in BytecodeEmitter to map names to locations.
class NameLocation
{
  public:
    enum class Kind : uint8_t
    {
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
    // Otherwise LOCALNO_LIMIT/ENVCOORD_SLOT_LIMIT.
    uint32_t slot_ : ENVCOORD_SLOT_BITS;

    static_assert(LOCALNO_BITS == ENVCOORD_SLOT_BITS,
                  "Frame and environment slots must be same sized.");

    NameLocation(Kind kind, BindingKind bindingKind,
                 uint8_t hops = UINT8_MAX, uint32_t slot = ENVCOORD_SLOT_LIMIT)
      : kind_(kind),
        bindingKind_(bindingKind),
        hops_(hops),
        slot_(slot)
    { }

  public:
    // Default constructor for InlineMap.
    NameLocation() = default;

    static NameLocation Dynamic() {
        return NameLocation();
    }

    static NameLocation Global(BindingKind bindKind) {
        MOZ_ASSERT(bindKind != BindingKind::FormalParameter);
        return NameLocation(Kind::Global, bindKind);
    }

    static NameLocation Intrinsic() {
        return NameLocation(Kind::Intrinsic, BindingKind::Var);
    }

    static NameLocation NamedLambdaCallee() {
        return NameLocation(Kind::NamedLambdaCallee, BindingKind::NamedLambdaCallee);
    }

    static NameLocation ArgumentSlot(uint16_t slot) {
        return NameLocation(Kind::ArgumentSlot, BindingKind::FormalParameter, 0, slot);
    }

    static NameLocation FrameSlot(BindingKind bindKind, uint32_t slot) {
        MOZ_ASSERT(slot < LOCALNO_LIMIT);
        return NameLocation(Kind::FrameSlot, bindKind, 0, slot);
    }

    static NameLocation EnvironmentCoordinate(BindingKind bindKind, uint8_t hops, uint32_t slot) {
        MOZ_ASSERT(slot < ENVCOORD_SLOT_LIMIT);
        return NameLocation(Kind::EnvironmentCoordinate, bindKind, hops, slot);
    }

    static NameLocation Import() {
        return NameLocation(Kind::Import, BindingKind::Import);
    }

    static NameLocation DynamicAnnexBVar() {
        return NameLocation(Kind::DynamicAnnexBVar, BindingKind::Var);
    }

    static NameLocation fromBinding(BindingKind bindKind, const BindingLocation& bl) {
        switch (bl.kind()) {
          case BindingLocation::Kind::Global:
            return Global(bindKind);
          case BindingLocation::Kind::Argument:
            return ArgumentSlot(bl.argumentSlot());
          case BindingLocation::Kind::Frame:
            return FrameSlot(bindKind, bl.slot());
          case BindingLocation::Kind::Environment:
            return EnvironmentCoordinate(bindKind, 0, bl.slot());
          case BindingLocation::Kind::Import:
            return Import();
          case BindingLocation::Kind::NamedLambdaCallee:
            return NamedLambdaCallee();
        }
        MOZ_CRASH("Bad BindingKind");
    }

    bool operator==(const NameLocation& other) const {
        return kind_ == other.kind_ && bindingKind_ == other.bindingKind_ &&
               hops_ == other.hops_ && slot_ == other.slot_;
    }

    bool operator!=(const NameLocation& other) const {
        return !(*this == other);
    }

    Kind kind() const {
        return kind_;
    }

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
        MOZ_ASSERT(kind_ == Kind::EnvironmentCoordinate);
        class EnvironmentCoordinate coord;
        coord.setHops(hops_);
        coord.setSlot(slot_);
        return coord;
    }

    BindingKind bindingKind() const {
        MOZ_ASSERT(kind_ != Kind::Dynamic);
        return bindingKind_;
    }

    bool isLexical() const {
        return BindingKindIsLexical(bindingKind());
    }

    bool isConst() const {
        return bindingKind() == BindingKind::Const;
    }

    bool hasKnownSlot() const {
        return kind_ == Kind::ArgumentSlot ||
               kind_ == Kind::FrameSlot ||
               kind_ == Kind::EnvironmentCoordinate;
    }
};

// This type is declared here for LazyScript::Create.
using AtomVector = Vector<JSAtom*, 24, SystemAllocPolicy>;

} // namespace frontend
} // namespace js

namespace mozilla {

template <>
struct IsPod<js::frontend::DeclaredNameInfo> : TrueType {};

template <>
struct IsPod<js::frontend::NameLocation> : TrueType {};

} // namespace mozilla

#endif // frontend_NameAnalysisTypes_h
