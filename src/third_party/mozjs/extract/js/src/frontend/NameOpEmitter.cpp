/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/NameOpEmitter.h"

#include "frontend/AbstractScopePtr.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/ParserAtom.h"  // ParserAtom
#include "frontend/SharedContext.h"
#include "frontend/TDZCheckCache.h"
#include "frontend/ValueUsage.h"
#include "js/Value.h"
#include "vm/Opcodes.h"

using namespace js;
using namespace js::frontend;

NameOpEmitter::NameOpEmitter(BytecodeEmitter* bce, TaggedParserAtomIndex name,
                             Kind kind)
    : bce_(bce), kind_(kind), name_(name), loc_(bce_->lookupName(name_)) {}

NameOpEmitter::NameOpEmitter(BytecodeEmitter* bce, TaggedParserAtomIndex name,
                             const NameLocation& loc, Kind kind)
    : bce_(bce), kind_(kind), name_(name), loc_(loc) {}

bool NameOpEmitter::emitGet() {
  MOZ_ASSERT(state_ == State::Start);

  bool needsImplicitThis = false;
  if (isCall()) {
    switch (loc_.kind()) {
      case NameLocation::Kind::Dynamic:
        if (bce_->needsImplicitThis()) {
          needsImplicitThis = true;
          break;
        }
        [[fallthrough]];
      case NameLocation::Kind::Global:
        MOZ_ASSERT(bce_->outermostScope().hasNonSyntacticScopeOnChain() ==
                   bce_->sc->hasNonSyntacticScope());
        needsImplicitThis = bce_->sc->hasNonSyntacticScope();
        break;
      case NameLocation::Kind::Intrinsic:
      case NameLocation::Kind::NamedLambdaCallee:
      case NameLocation::Kind::Import:
      case NameLocation::Kind::ArgumentSlot:
      case NameLocation::Kind::FrameSlot:
      case NameLocation::Kind::EnvironmentCoordinate:
      case NameLocation::Kind::DebugEnvironmentCoordinate:
      case NameLocation::Kind::DynamicAnnexBVar:
        break;
    }
  }

  switch (loc_.kind()) {
    case NameLocation::Kind::Global:
      MOZ_ASSERT(bce_->outermostScope().hasNonSyntacticScopeOnChain() ==
                 bce_->sc->hasNonSyntacticScope());
      if (!bce_->sc->hasNonSyntacticScope()) {
        MOZ_ASSERT(!needsImplicitThis);

        // Some names on the global are not configurable and have fixed values
        // which we can emit instead.
        if (name_ == TaggedParserAtomIndex::WellKnown::undefined()) {
          if (!bce_->emit1(JSOp::Undefined)) {
            return false;
          }
        } else if (name_ == TaggedParserAtomIndex::WellKnown::NaN()) {
          if (!bce_->emitDouble(JS::GenericNaN())) {
            return false;
          }
        } else if (name_ == TaggedParserAtomIndex::WellKnown::Infinity()) {
          if (!bce_->emitDouble(JS::Infinity())) {
            return false;
          }
        } else {
          if (!bce_->emitAtomOp(JSOp::GetGName, name_)) {
            //      [stack] VAL
            return false;
          }
        }
        break;
      }
      [[fallthrough]];
    case NameLocation::Kind::Dynamic:
      if (needsImplicitThis) {
        if (!bce_->emitAtomOp(JSOp::BindName, name_)) {
          //        [stack] ENV
          return false;
        }
        if (!bce_->emit1(JSOp::Dup)) {
          //        [stack] ENV ENV
          return false;
        }
        if (!bce_->emitAtomOp(JSOp::GetBoundName, name_)) {
          //        [stack] ENV V
          return false;
        }
      } else {
        if (!bce_->emitAtomOp(JSOp::GetName, name_)) {
          //        [stack] VAL
          return false;
        }
      }
      break;
    case NameLocation::Kind::Intrinsic:
      if (name_ == TaggedParserAtomIndex::WellKnown::undefined()) {
        if (!bce_->emit1(JSOp::Undefined)) {
          //        [stack] Undefined
          return false;
        }
      } else {
        if (!bce_->emitAtomOp(JSOp::GetIntrinsic, name_)) {
          //        [stack] VAL
          return false;
        }
      }
      break;
    case NameLocation::Kind::NamedLambdaCallee:
      if (!bce_->emit1(JSOp::Callee)) {
        //          [stack] VAL
        return false;
      }
      break;
    case NameLocation::Kind::Import:
      if (!bce_->emitAtomOp(JSOp::GetImport, name_)) {
        //          [stack] VAL
        return false;
      }
      break;
    case NameLocation::Kind::ArgumentSlot:
      if (!bce_->emitArgOp(JSOp::GetArg, loc_.argumentSlot())) {
        //          [stack] VAL
        return false;
      }
      break;
    case NameLocation::Kind::FrameSlot:
      if (!bce_->emitLocalOp(JSOp::GetLocal, loc_.frameSlot())) {
        //          [stack] VAL
        return false;
      }
      if (loc_.isLexical()) {
        if (!bce_->emitTDZCheckIfNeeded(name_, loc_, ValueIsOnStack::Yes)) {
          //        [stack] VAL
          return false;
        }
      }
      break;
    case NameLocation::Kind::EnvironmentCoordinate:
    case NameLocation::Kind::DebugEnvironmentCoordinate:
      if (!bce_->emitEnvCoordOp(
              loc_.kind() == NameLocation::Kind::EnvironmentCoordinate
                  ? JSOp::GetAliasedVar
                  : JSOp::GetAliasedDebugVar,
              loc_.environmentCoordinate())) {
        //          [stack] VAL
        return false;
      }
      if (loc_.isLexical()) {
        if (!bce_->emitTDZCheckIfNeeded(name_, loc_, ValueIsOnStack::Yes)) {
          //        [stack] VAL
          return false;
        }
      }
      break;
    case NameLocation::Kind::DynamicAnnexBVar:
      MOZ_CRASH(
          "Synthesized vars for Annex B.3.3 should only be used in "
          "initialization");
  }

  if (isCall()) {
    switch (loc_.kind()) {
      case NameLocation::Kind::Dynamic:
      case NameLocation::Kind::Global:
        MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);
        if (needsImplicitThis) {
          if (!bce_->emit1(JSOp::Swap)) {
            //      [stack] CALLEE ENV
            return false;
          }
          if (!bce_->emit1(JSOp::ImplicitThis)) {
            //      [stack] CALLEE THIS
            return false;
          }
        } else {
          if (!bce_->emit1(JSOp::Undefined)) {
            //      [stack] CALLEE UNDEF
            return false;
          }
        }
        break;
      case NameLocation::Kind::Intrinsic:
      case NameLocation::Kind::NamedLambdaCallee:
      case NameLocation::Kind::Import:
      case NameLocation::Kind::ArgumentSlot:
      case NameLocation::Kind::FrameSlot:
      case NameLocation::Kind::EnvironmentCoordinate:
        if (bce_->emitterMode == BytecodeEmitter::SelfHosting) {
          if (!bce_->emitDebugCheckSelfHosted()) {
            //      [stack] CALLEE
            return false;
          }
        }
        if (!bce_->emit1(JSOp::Undefined)) {
          //        [stack] CALLEE UNDEF
          return false;
        }
        break;
      case NameLocation::Kind::DebugEnvironmentCoordinate:
        MOZ_CRASH(
            "DebugEnvironmentCoordinate should only be used to get the private "
            "brand, and so should never call.");
        break;
      case NameLocation::Kind::DynamicAnnexBVar:
        MOZ_CRASH(
            "Synthesized vars for Annex B.3.3 should only be used in "
            "initialization");
    }
  }

#ifdef DEBUG
  state_ = State::Get;
#endif
  return true;
}

bool NameOpEmitter::prepareForRhs() {
  MOZ_ASSERT(state_ == State::Start);

  switch (loc_.kind()) {
    case NameLocation::Kind::Dynamic:
    case NameLocation::Kind::Import:
      if (!bce_->makeAtomIndex(name_, ParserAtom::Atomize::Yes, &atomIndex_)) {
        return false;
      }
      if (!bce_->emitAtomOp(JSOp::BindUnqualifiedName, atomIndex_)) {
        //          [stack] ENV
        return false;
      }
      emittedBindOp_ = true;
      break;
    case NameLocation::Kind::DynamicAnnexBVar:
      // Annex B vars always go on the nearest variable environment, even if
      // lexical environments in between contain same-named bindings.
      if (!bce_->emit1(JSOp::BindVar)) {
        //          [stack] ENV
        return false;
      }
      emittedBindOp_ = true;
      break;
    case NameLocation::Kind::Global:
      if (!bce_->makeAtomIndex(name_, ParserAtom::Atomize::Yes, &atomIndex_)) {
        return false;
      }
      MOZ_ASSERT(bce_->outermostScope().hasNonSyntacticScopeOnChain() ==
                 bce_->sc->hasNonSyntacticScope());

      if (loc_.isLexical() && isInitialize()) {
        // InitGLexical always gets the global lexical scope. It doesn't
        // need a BindUnqualifiedName/BindUnqualifiedGName.
        MOZ_ASSERT(bce_->innermostScope().is<GlobalScope>());
      } else {
        JSOp op;
        if (bce_->sc->hasNonSyntacticScope()) {
          op = JSOp::BindUnqualifiedName;
        } else {
          op = JSOp::BindUnqualifiedGName;
        }
        if (!bce_->emitAtomOp(op, atomIndex_)) {
          //        [stack] ENV
          return false;
        }
        emittedBindOp_ = true;
      }
      break;
    case NameLocation::Kind::Intrinsic:
    case NameLocation::Kind::NamedLambdaCallee:
    case NameLocation::Kind::ArgumentSlot:
    case NameLocation::Kind::FrameSlot:
    case NameLocation::Kind::DebugEnvironmentCoordinate:
    case NameLocation::Kind::EnvironmentCoordinate:
      break;
  }

  // For compound assignments, first get the LHS value, then emit
  // the RHS and the op.
  if (isCompoundAssignment() || isIncDec()) {
    if (loc_.kind() == NameLocation::Kind::Dynamic) {
      // For dynamic accesses we need to emit GetBoundName instead of
      // GetName for correctness: looking up @@unscopables on the
      // environment chain (due to 'with' environments) must only happen
      // once.
      //
      // GetBoundName uses the environment already pushed on the stack
      // from the earlier BindName.
      if (!bce_->emit1(JSOp::Dup)) {
        //          [stack] ENV ENV
        return false;
      }
      if (!bce_->emitAtomOp(JSOp::GetBoundName, atomIndex_)) {
        //          [stack] ENV V
        return false;
      }
    } else {
      if (!emitGet()) {
        //          [stack] ENV? V
        return false;
      }
    }
  }

#ifdef DEBUG
  state_ = State::Rhs;
#endif
  return true;
}

JSOp NameOpEmitter::strictifySetNameOp(JSOp op) const {
  switch (op) {
    case JSOp::SetName:
      if (bce_->sc->strict()) {
        op = JSOp::StrictSetName;
      }
      break;
    case JSOp::SetGName:
      if (bce_->sc->strict()) {
        op = JSOp::StrictSetGName;
      }
      break;
    default:
      MOZ_CRASH("Invalid SetName op");
  }
  return op;
}

#if defined(__clang__) && defined(XP_WIN) && \
    (defined(_M_X64) || defined(__x86_64__))
// Work around a CPU bug. See bug 1524257.
__attribute__((__aligned__(32)))
#endif
bool NameOpEmitter::emitAssignment() {
  MOZ_ASSERT(state_ == State::Rhs);

  //                [stack] # If emittedBindOp_
  //                [stack] ENV V
  //                [stack] # else
  //                [stack] V

  switch (loc_.kind()) {
    case NameLocation::Kind::Dynamic:
    case NameLocation::Kind::Import:
      MOZ_ASSERT(emittedBindOp_);
      if (!bce_->emitAtomOp(strictifySetNameOp(JSOp::SetName), atomIndex_)) {
        return false;
      }
      break;
    case NameLocation::Kind::DynamicAnnexBVar:
      MOZ_ASSERT(emittedBindOp_);
      if (!bce_->emitAtomOp(strictifySetNameOp(JSOp::SetName), name_)) {
        return false;
      }
      break;
    case NameLocation::Kind::Global: {
      JSOp op;
      if (emittedBindOp_) {
        MOZ_ASSERT(bce_->outermostScope().hasNonSyntacticScopeOnChain() ==
                   bce_->sc->hasNonSyntacticScope());
        if (bce_->sc->hasNonSyntacticScope()) {
          op = strictifySetNameOp(JSOp::SetName);
        } else {
          op = strictifySetNameOp(JSOp::SetGName);
        }
      } else {
        op = JSOp::InitGLexical;
      }
      if (!bce_->emitAtomOp(op, atomIndex_)) {
        return false;
      }
      break;
    }
    case NameLocation::Kind::Intrinsic:
      if (!bce_->emitAtomOp(JSOp::SetIntrinsic, name_)) {
        return false;
      }
      break;
    case NameLocation::Kind::NamedLambdaCallee:
      // Assigning to the named lambda is a no-op in sloppy mode but
      // throws in strict mode.
      if (bce_->sc->strict()) {
        if (!bce_->emitAtomOp(JSOp::ThrowSetConst, name_)) {
          return false;
        }
      }
      break;
    case NameLocation::Kind::ArgumentSlot:
      if (!bce_->emitArgOp(JSOp::SetArg, loc_.argumentSlot())) {
        return false;
      }
      break;
    case NameLocation::Kind::FrameSlot: {
      JSOp op = JSOp::SetLocal;
      // Lexicals, Synthetics and Private Methods have very similar handling
      // around a variety of areas, including initialization.
      if (loc_.isLexical() || loc_.isPrivateMethod() || loc_.isSynthetic()) {
        if (isInitialize()) {
          op = JSOp::InitLexical;
        } else {
          if (loc_.isConst()) {
            op = JSOp::ThrowSetConst;
          }
          if (!bce_->emitTDZCheckIfNeeded(name_, loc_, ValueIsOnStack::No)) {
            return false;
          }
        }
      }
      if (op == JSOp::ThrowSetConst) {
        if (!bce_->emitAtomOp(op, name_)) {
          return false;
        }
      } else {
        if (!bce_->emitLocalOp(op, loc_.frameSlot())) {
          return false;
        }
      }
      if (op == JSOp::InitLexical) {
        if (!bce_->innermostTDZCheckCache->noteTDZCheck(bce_, name_,
                                                        DontCheckTDZ)) {
          return false;
        }
      }
      break;
    }
    case NameLocation::Kind::EnvironmentCoordinate: {
      JSOp op = JSOp::SetAliasedVar;
      // Lexicals, Synthetics and Private Methods have very similar handling
      // around a variety of areas, including initialization.
      if (loc_.isLexical() || loc_.isPrivateMethod() || loc_.isSynthetic()) {
        if (isInitialize()) {
          op = JSOp::InitAliasedLexical;
        } else {
          if (loc_.isConst()) {
            op = JSOp::ThrowSetConst;
          }
          if (!bce_->emitTDZCheckIfNeeded(name_, loc_, ValueIsOnStack::No)) {
            return false;
          }
        }
      } else if (loc_.bindingKind() == BindingKind::NamedLambdaCallee) {
        // Assigning to the named lambda is a no-op in sloppy mode and throws
        // in strict mode.
        if (!bce_->sc->strict()) {
          break;
        }
        op = JSOp::ThrowSetConst;
      }
      if (op == JSOp::ThrowSetConst) {
        if (!bce_->emitAtomOp(op, name_)) {
          return false;
        }
      } else {
        if (!bce_->emitEnvCoordOp(op, loc_.environmentCoordinate())) {
          return false;
        }
      }
      if (op == JSOp::InitAliasedLexical) {
        if (!bce_->innermostTDZCheckCache->noteTDZCheck(bce_, name_,
                                                        DontCheckTDZ)) {
          return false;
        }
      }
      break;
    }
    case NameLocation::Kind::DebugEnvironmentCoordinate:
      MOZ_CRASH("Shouldn't be assigning to a private brand");
      break;
  }

#ifdef DEBUG
  state_ = State::Assignment;
#endif
  return true;
}

bool NameOpEmitter::emitIncDec(ValueUsage valueUsage) {
  MOZ_ASSERT(state_ == State::Start);

  JSOp incOp = isInc() ? JSOp::Inc : JSOp::Dec;
  if (!prepareForRhs()) {
    //              [stack] ENV? V
    return false;
  }
  if (!bce_->emit1(JSOp::ToNumeric)) {
    //              [stack] ENV? N
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] ENV? N N
      return false;
    }
  }
  if (!bce_->emit1(incOp)) {
    //              [stack] ENV? N? N+1
    return false;
  }
  if (isPostIncDec() && emittedBindOp() &&
      valueUsage == ValueUsage::WantValue) {
    if (!bce_->emit2(JSOp::Pick, 2)) {
      //            [stack] N N+1 ENV
      return false;
    }
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] N ENV N+1
      return false;
    }
  }
  if (!emitAssignment()) {
    //              [stack] N? N+1
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] N
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::IncDec;
#endif
  return true;
}
