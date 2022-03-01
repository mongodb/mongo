/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/NameOpEmitter.h"

#include "frontend/AbstractScopePtr.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/SharedContext.h"
#include "frontend/TDZCheckCache.h"
#include "vm/Opcodes.h"
#include "vm/Scope.h"
#include "vm/StringType.h"

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

  switch (loc_.kind()) {
    case NameLocation::Kind::Dynamic:
      if (!bce_->emitAtomOp(JSOp::GetName, name_)) {
        //          [stack] VAL
        return false;
      }
      break;
    case NameLocation::Kind::Global:
      if (!bce_->emitAtomOp(JSOp::GetGName, name_)) {
        //          [stack] VAL
        return false;
      }
      break;
    case NameLocation::Kind::Intrinsic:
      if (!bce_->emitAtomOp(JSOp::GetIntrinsic, name_)) {
        //          [stack] VAL
        return false;
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
      case NameLocation::Kind::Dynamic: {
        JSOp thisOp = bce_->needsImplicitThis() ? JSOp::ImplicitThis
                                                : JSOp::GImplicitThis;
        if (!bce_->emitAtomOp(thisOp, name_)) {
          //        [stack] CALLEE THIS
          return false;
        }
        break;
      }
      case NameLocation::Kind::Global:
        if (!bce_->emitAtomOp(JSOp::GImplicitThis, name_)) {
          //        [stack] CALLEE THIS
          return false;
        }
        break;
      case NameLocation::Kind::Intrinsic:
      case NameLocation::Kind::NamedLambdaCallee:
      case NameLocation::Kind::Import:
      case NameLocation::Kind::ArgumentSlot:
      case NameLocation::Kind::FrameSlot:
      case NameLocation::Kind::EnvironmentCoordinate:
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
    case NameLocation::Kind::DynamicAnnexBVar:
      if (!bce_->makeAtomIndex(name_, &atomIndex_)) {
        return false;
      }
      if (loc_.kind() == NameLocation::Kind::DynamicAnnexBVar) {
        // Annex B vars always go on the nearest variable environment,
        // even if lexical environments in between contain same-named
        // bindings.
        if (!bce_->emit1(JSOp::BindVar)) {
          //        [stack] ENV
          return false;
        }
      } else {
        if (!bce_->emitAtomOp(JSOp::BindName, atomIndex_)) {
          //        [stack] ENV
          return false;
        }
      }
      emittedBindOp_ = true;
      break;
    case NameLocation::Kind::Global:
      if (!bce_->makeAtomIndex(name_, &atomIndex_)) {
        return false;
      }
      if (loc_.isLexical() && isInitialize()) {
        // InitGLexical always gets the global lexical scope. It doesn't
        // need a BindGName.
        MOZ_ASSERT(bce_->innermostScope().is<GlobalScope>());
      } else {
        if (!bce_->emitAtomOp(JSOp::BindGName, atomIndex_)) {
          //        [stack] ENV
          return false;
        }
        emittedBindOp_ = true;
      }
      break;
    case NameLocation::Kind::Intrinsic:
      break;
    case NameLocation::Kind::NamedLambdaCallee:
      break;
    case NameLocation::Kind::ArgumentSlot:
      break;
    case NameLocation::Kind::FrameSlot:
      break;
    case NameLocation::Kind::DebugEnvironmentCoordinate:
      break;
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
      if (!bce_->emitAtomOp(JSOp::GetBoundName, name_)) {
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

#if defined(__clang__) && defined(XP_WIN) && \
    (defined(_M_X64) || defined(__x86_64__))
// Work around a CPU bug. See bug 1524257.
__attribute__((__aligned__(32)))
#endif
bool NameOpEmitter::emitAssignment() {
  MOZ_ASSERT(state_ == State::Rhs);

  switch (loc_.kind()) {
    case NameLocation::Kind::Dynamic:
    case NameLocation::Kind::Import:
    case NameLocation::Kind::DynamicAnnexBVar:
      if (!bce_->emitAtomOp(bce_->strictifySetNameOp(JSOp::SetName),
                            atomIndex_)) {
        return false;
      }
      break;
    case NameLocation::Kind::Global: {
      JSOp op;
      if (emittedBindOp_) {
        op = bce_->strictifySetNameOp(JSOp::SetGName);
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
      }
      if (loc_.bindingKind() == BindingKind::NamedLambdaCallee) {
        // Assigning to the named lambda is a no-op in sloppy mode and throws
        // in strict mode.
        op = JSOp::ThrowSetConst;
        if (bce_->sc->strict()) {
          if (!bce_->emitAtomOp(op, name_)) {
            return false;
          }
        }
      } else {
        if (op == JSOp::ThrowSetConst) {
          if (!bce_->emitAtomOp(op, name_)) {
            return false;
          }
        } else {
          if (!bce_->emitEnvCoordOp(op, loc_.environmentCoordinate())) {
            return false;
          }
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

bool NameOpEmitter::emitIncDec() {
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
  if (isPostIncDec()) {
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] ENV? N? N
      return false;
    }
  }
  if (!bce_->emit1(incOp)) {
    //              [stack] ENV? N? N+1
    return false;
  }
  if (isPostIncDec() && emittedBindOp()) {
    if (!bce_->emit2(JSOp::Pick, 2)) {
      //            [stack] N? N+1 ENV?
      return false;
    }
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] N? ENV? N+1
      return false;
    }
  }
  if (!emitAssignment()) {
    //              [stack] N? N+1
    return false;
  }
  if (isPostIncDec()) {
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
