/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonIC.h"

#include "jit/CacheIRCompiler.h"
#include "jit/VMFunctions.h"
#include "util/DiagnosticAssertions.h"
#include "vm/EqualityOperations.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;
using namespace js::jit;

void IonIC::resetCodeRaw(IonScript* ionScript) {
  codeRaw_ = fallbackAddr(ionScript);
}

uint8_t* IonIC::fallbackAddr(IonScript* ionScript) const {
  return ionScript->method()->raw() + fallbackOffset_;
}

uint8_t* IonIC::rejoinAddr(IonScript* ionScript) const {
  return ionScript->method()->raw() + rejoinOffset_;
}

Register IonIC::scratchRegisterForEntryJump() {
  switch (kind_) {
    case CacheKind::GetProp:
    case CacheKind::GetElem:
      return asGetPropertyIC()->output().scratchReg();
    case CacheKind::GetPropSuper:
    case CacheKind::GetElemSuper:
      return asGetPropSuperIC()->output().scratchReg();
    case CacheKind::SetProp:
    case CacheKind::SetElem:
      return asSetPropertyIC()->temp();
    case CacheKind::GetName:
      return asGetNameIC()->temp();
    case CacheKind::BindName:
      return asBindNameIC()->temp();
    case CacheKind::In:
      return asInIC()->temp();
    case CacheKind::HasOwn:
      return asHasOwnIC()->output();
    case CacheKind::CheckPrivateField:
      return asCheckPrivateFieldIC()->output();
    case CacheKind::GetIterator:
      return asGetIteratorIC()->temp1();
    case CacheKind::OptimizeSpreadCall:
      return asOptimizeSpreadCallIC()->temp();
    case CacheKind::InstanceOf:
      return asInstanceOfIC()->output();
    case CacheKind::UnaryArith:
      return asUnaryArithIC()->output().scratchReg();
    case CacheKind::ToPropertyKey:
      return asToPropertyKeyIC()->output().scratchReg();
    case CacheKind::BinaryArith:
      return asBinaryArithIC()->output().scratchReg();
    case CacheKind::Compare:
      return asCompareIC()->output();
    case CacheKind::Call:
    case CacheKind::TypeOf:
    case CacheKind::ToBool:
    case CacheKind::GetIntrinsic:
    case CacheKind::NewArray:
    case CacheKind::NewObject:
      MOZ_CRASH("Unsupported IC");
  }

  MOZ_CRASH("Invalid kind");
}

void IonIC::discardStubs(Zone* zone, IonScript* ionScript) {
  if (firstStub_) {
    // We are removing edges from IonIC to gcthings. Perform a write barrier to
    // let the GC know about those edges.
    PreWriteBarrier(zone, ionScript);
  }

#ifdef JS_CRASH_DIAGNOSTICS
  IonICStub* stub = firstStub_;
  while (stub) {
    IonICStub* next = stub->next();
    stub->poison();
    stub = next;
  }
#endif

  firstStub_ = nullptr;
  resetCodeRaw(ionScript);
  state_.trackUnlinkedAllStubs();
}

void IonIC::reset(Zone* zone, IonScript* ionScript) {
  discardStubs(zone, ionScript);
  state_.reset();
}

void IonIC::trace(JSTracer* trc, IonScript* ionScript) {
  if (script_) {
    TraceManuallyBarrieredEdge(trc, &script_, "IonIC::script_");
  }

  uint8_t* nextCodeRaw = codeRaw_;
  for (IonICStub* stub = firstStub_; stub; stub = stub->next()) {
    JitCode* code = JitCode::FromExecutable(nextCodeRaw);
    TraceManuallyBarrieredEdge(trc, &code, "ion-ic-code");

    TraceCacheIRStub(trc, stub, stub->stubInfo());

    nextCodeRaw = stub->nextCodeRaw();
  }

  MOZ_ASSERT(nextCodeRaw == fallbackAddr(ionScript));
}

// This helper handles ICState updates/transitions while attaching CacheIR
// stubs.
template <typename IRGenerator, typename... Args>
static void TryAttachIonStub(JSContext* cx, IonIC* ic, IonScript* ionScript,
                             Args&&... args) {
  if (ic->state().maybeTransition()) {
    ic->discardStubs(cx->zone(), ionScript);
  }

  if (ic->state().canAttachStub()) {
    RootedScript script(cx, ic->script());
    bool attached = false;
    IRGenerator gen(cx, script, ic->pc(), ic->state(),
                    std::forward<Args>(args)...);
    switch (gen.tryAttachStub()) {
      case AttachDecision::Attach:
        ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript,
                              &attached);
        break;
      case AttachDecision::NoAction:
        break;
      case AttachDecision::TemporarilyUnoptimizable:
        attached = true;
        break;
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("Not expected in generic TryAttachIonStub");
        break;
    }
    if (!attached) {
      ic->state().trackNotAttached();
    }
  }
}

/* static */
bool IonGetPropertyIC::update(JSContext* cx, HandleScript outerScript,
                              IonGetPropertyIC* ic, HandleValue val,
                              HandleValue idVal, MutableHandleValue res) {
  IonScript* ionScript = outerScript->ionScript();

  // Optimized-arguments and other magic values must not escape to Ion ICs.
  MOZ_ASSERT(!val.isMagic());

  TryAttachIonStub<GetPropIRGenerator>(cx, ic, ionScript, ic->kind(), val,
                                       idVal);

  if (ic->kind() == CacheKind::GetProp) {
    RootedPropertyName name(cx, idVal.toString()->asAtom().asPropertyName());
    if (!GetProperty(cx, val, name, res)) {
      return false;
    }
  } else {
    MOZ_ASSERT(ic->kind() == CacheKind::GetElem);
    if (!GetElementOperation(cx, val, idVal, res)) {
      return false;
    }
  }

  return true;
}

/* static */
bool IonGetPropSuperIC::update(JSContext* cx, HandleScript outerScript,
                               IonGetPropSuperIC* ic, HandleObject obj,
                               HandleValue receiver, HandleValue idVal,
                               MutableHandleValue res) {
  IonScript* ionScript = outerScript->ionScript();

  if (ic->state().maybeTransition()) {
    ic->discardStubs(cx->zone(), ionScript);
  }

  RootedValue val(cx, ObjectValue(*obj));

  TryAttachIonStub<GetPropIRGenerator>(cx, ic, ionScript, ic->kind(), val,
                                       idVal);

  if (ic->kind() == CacheKind::GetPropSuper) {
    RootedPropertyName name(cx, idVal.toString()->asAtom().asPropertyName());
    if (!GetProperty(cx, obj, receiver, name, res)) {
      return false;
    }
  } else {
    MOZ_ASSERT(ic->kind() == CacheKind::GetElemSuper);

    JSOp op = JSOp(*ic->pc());
    MOZ_ASSERT(op == JSOp::GetElemSuper);

    if (!GetObjectElementOperation(cx, op, obj, receiver, idVal, res)) {
      return false;
    }
  }

  return true;
}

/* static */
bool IonSetPropertyIC::update(JSContext* cx, HandleScript outerScript,
                              IonSetPropertyIC* ic, HandleObject obj,
                              HandleValue idVal, HandleValue rhs) {
  using DeferType = SetPropIRGenerator::DeferType;

  RootedShape oldShape(cx);
  IonScript* ionScript = outerScript->ionScript();

  bool attached = false;
  DeferType deferType = DeferType::None;

  if (ic->state().maybeTransition()) {
    ic->discardStubs(cx->zone(), ionScript);
  }

  if (ic->state().canAttachStub()) {
    oldShape = obj->shape();

    RootedValue objv(cx, ObjectValue(*obj));
    RootedScript script(cx, ic->script());
    jsbytecode* pc = ic->pc();

    SetPropIRGenerator gen(cx, script, pc, ic->kind(), ic->state(), objv, idVal,
                           rhs);
    switch (gen.tryAttachStub()) {
      case AttachDecision::Attach:
        ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript,
                              &attached);
        break;
      case AttachDecision::NoAction:
        break;
      case AttachDecision::TemporarilyUnoptimizable:
        attached = true;
        break;
      case AttachDecision::Deferred:
        deferType = gen.deferType();
        MOZ_ASSERT(deferType != DeferType::None);
        break;
    }
  }

  jsbytecode* pc = ic->pc();
  if (ic->kind() == CacheKind::SetElem) {
    if (JSOp(*pc) == JSOp::InitElemInc) {
      if (!InitElemIncOperation(cx, obj.as<ArrayObject>(), idVal.toInt32(),
                                rhs)) {
        return false;
      }
    } else if (IsPropertyInitOp(JSOp(*pc))) {
      if (!InitElemOperation(cx, pc, obj, idVal, rhs)) {
        return false;
      }
    } else {
      MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));
      if (!SetObjectElement(cx, obj, idVal, rhs, ic->strict())) {
        return false;
      }
    }
  } else {
    MOZ_ASSERT(ic->kind() == CacheKind::SetProp);

    if (JSOp(*pc) == JSOp::InitGLexical) {
      RootedScript script(cx, ic->script());
      MOZ_ASSERT(!script->hasNonSyntacticScope());
      InitGlobalLexicalOperation(cx, &cx->global()->lexicalEnvironment(),
                                 script, pc, rhs);
    } else if (IsPropertyInitOp(JSOp(*pc))) {
      // This might be a JSOp::InitElem op with a constant string id. We
      // can't call InitPropertyOperation here as that function is
      // specialized for JSOp::Init*Prop (it does not support arbitrary
      // objects that might show up here).
      if (!InitElemOperation(cx, pc, obj, idVal, rhs)) {
        return false;
      }
    } else {
      MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));
      RootedPropertyName name(cx, idVal.toString()->asAtom().asPropertyName());
      if (!SetProperty(cx, obj, name, rhs, ic->strict(), pc)) {
        return false;
      }
    }
  }

  if (attached) {
    return true;
  }

  // The SetProperty call might have entered this IC recursively, so try
  // to transition.
  if (ic->state().maybeTransition()) {
    ic->discardStubs(cx->zone(), ionScript);
  }

  bool canAttachStub = ic->state().canAttachStub();
  if (deferType != DeferType::None && canAttachStub) {
    RootedValue objv(cx, ObjectValue(*obj));
    RootedScript script(cx, ic->script());
    jsbytecode* pc = ic->pc();
    SetPropIRGenerator gen(cx, script, pc, ic->kind(), ic->state(), objv, idVal,
                           rhs);
    MOZ_ASSERT(deferType == DeferType::AddSlot);
    AttachDecision decision = gen.tryAttachAddSlotStub(oldShape);

    switch (decision) {
      case AttachDecision::Attach:
        ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript,
                              &attached);
        break;
      case AttachDecision::NoAction:
        gen.trackAttached(IRGenerator::NotAttached);
        break;
      case AttachDecision::TemporarilyUnoptimizable:
      case AttachDecision::Deferred:
        MOZ_ASSERT_UNREACHABLE("Invalid attach result");
        break;
    }
  }
  if (!attached && canAttachStub) {
    ic->state().trackNotAttached();
  }

  return true;
}

/* static */
bool IonGetNameIC::update(JSContext* cx, HandleScript outerScript,
                          IonGetNameIC* ic, HandleObject envChain,
                          MutableHandleValue res) {
  IonScript* ionScript = outerScript->ionScript();
  jsbytecode* pc = ic->pc();
  RootedPropertyName name(cx, ic->script()->getName(pc));

  TryAttachIonStub<GetNameIRGenerator>(cx, ic, ionScript, envChain, name);

  RootedObject obj(cx);
  RootedObject holder(cx);
  PropertyResult prop;
  if (!LookupName(cx, name, envChain, &obj, &holder, &prop)) {
    return false;
  }

  if (JSOp(*GetNextPc(pc)) == JSOp::Typeof) {
    return FetchName<GetNameMode::TypeOf>(cx, obj, holder, name, prop, res);
  }

  return FetchName<GetNameMode::Normal>(cx, obj, holder, name, prop, res);
}

/* static */
JSObject* IonBindNameIC::update(JSContext* cx, HandleScript outerScript,
                                IonBindNameIC* ic, HandleObject envChain) {
  IonScript* ionScript = outerScript->ionScript();
  jsbytecode* pc = ic->pc();
  RootedPropertyName name(cx, ic->script()->getName(pc));

  TryAttachIonStub<BindNameIRGenerator>(cx, ic, ionScript, envChain, name);

  RootedObject holder(cx);
  if (!LookupNameUnqualified(cx, name, envChain, &holder)) {
    return nullptr;
  }

  return holder;
}

/* static */
JSObject* IonGetIteratorIC::update(JSContext* cx, HandleScript outerScript,
                                   IonGetIteratorIC* ic, HandleValue value) {
  IonScript* ionScript = outerScript->ionScript();

  TryAttachIonStub<GetIteratorIRGenerator>(cx, ic, ionScript, value);

  return ValueToIterator(cx, value);
}

/* static */
bool IonOptimizeSpreadCallIC::update(JSContext* cx, HandleScript outerScript,
                                     IonOptimizeSpreadCallIC* ic,
                                     HandleValue value, bool* result) {
  IonScript* ionScript = outerScript->ionScript();

  TryAttachIonStub<OptimizeSpreadCallIRGenerator>(cx, ic, ionScript, value);

  return OptimizeSpreadCall(cx, value, result);
}

/* static */
bool IonHasOwnIC::update(JSContext* cx, HandleScript outerScript,
                         IonHasOwnIC* ic, HandleValue val, HandleValue idVal,
                         int32_t* res) {
  IonScript* ionScript = outerScript->ionScript();

  TryAttachIonStub<HasPropIRGenerator>(cx, ic, ionScript, CacheKind::HasOwn,
                                       idVal, val);

  bool found;
  if (!HasOwnProperty(cx, val, idVal, &found)) {
    return false;
  }

  *res = found;
  return true;
}

/* static */
bool IonCheckPrivateFieldIC::update(JSContext* cx, HandleScript outerScript,
                                    IonCheckPrivateFieldIC* ic, HandleValue val,
                                    HandleValue idVal, bool* res) {
  IonScript* ionScript = outerScript->ionScript();
  jsbytecode* pc = ic->pc();

  TryAttachIonStub<CheckPrivateFieldIRGenerator>(
      cx, ic, ionScript, CacheKind::CheckPrivateField, idVal, val);

  return CheckPrivateFieldOperation(cx, pc, val, idVal, res);
}

/* static */
bool IonInIC::update(JSContext* cx, HandleScript outerScript, IonInIC* ic,
                     HandleValue key, HandleObject obj, bool* res) {
  IonScript* ionScript = outerScript->ionScript();
  RootedValue objV(cx, ObjectValue(*obj));

  TryAttachIonStub<HasPropIRGenerator>(cx, ic, ionScript, CacheKind::In, key,
                                       objV);

  return OperatorIn(cx, key, obj, res);
}
/* static */
bool IonInstanceOfIC::update(JSContext* cx, HandleScript outerScript,
                             IonInstanceOfIC* ic, HandleValue lhs,
                             HandleObject rhs, bool* res) {
  IonScript* ionScript = outerScript->ionScript();

  TryAttachIonStub<InstanceOfIRGenerator>(cx, ic, ionScript, lhs, rhs);

  return HasInstance(cx, rhs, lhs, res);
}

/*  static */
bool IonToPropertyKeyIC::update(JSContext* cx, HandleScript outerScript,
                                IonToPropertyKeyIC* ic, HandleValue val,
                                MutableHandleValue res) {
  IonScript* ionScript = outerScript->ionScript();

  TryAttachIonStub<ToPropertyKeyIRGenerator>(cx, ic, ionScript, val);

  return ToPropertyKeyOperation(cx, val, res);
}

/*  static */
bool IonUnaryArithIC::update(JSContext* cx, HandleScript outerScript,
                             IonUnaryArithIC* ic, HandleValue val,
                             MutableHandleValue res) {
  IonScript* ionScript = outerScript->ionScript();
  RootedScript script(cx, ic->script());
  jsbytecode* pc = ic->pc();
  JSOp op = JSOp(*pc);

  switch (op) {
    case JSOp::BitNot: {
      res.set(val);
      if (!BitNot(cx, res, res)) {
        return false;
      }
      break;
    }
    case JSOp::Pos: {
      res.set(val);
      if (!ToNumber(cx, res)) {
        return false;
      }
      break;
    }
    case JSOp::Neg: {
      res.set(val);
      if (!NegOperation(cx, res, res)) {
        return false;
      }
      break;
    }
    case JSOp::Inc: {
      if (!IncOperation(cx, val, res)) {
        return false;
      }
      break;
    }
    case JSOp::Dec: {
      if (!DecOperation(cx, val, res)) {
        return false;
      }
      break;
    }
    case JSOp::ToNumeric: {
      res.set(val);
      if (!ToNumeric(cx, res)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH("Unexpected op");
  }
  MOZ_ASSERT(res.isNumeric());

  TryAttachIonStub<UnaryArithIRGenerator>(cx, ic, ionScript, op, val, res);

  return true;
}

/* static */
bool IonBinaryArithIC::update(JSContext* cx, HandleScript outerScript,
                              IonBinaryArithIC* ic, HandleValue lhs,
                              HandleValue rhs, MutableHandleValue ret) {
  IonScript* ionScript = outerScript->ionScript();
  RootedScript script(cx, ic->script());
  jsbytecode* pc = ic->pc();
  JSOp op = JSOp(*pc);

  // Don't pass lhs/rhs directly, we need the original values when
  // generating stubs.
  RootedValue lhsCopy(cx, lhs);
  RootedValue rhsCopy(cx, rhs);

  // Perform the compare operation.
  switch (op) {
    case JSOp::Add:
      // Do an add.
      if (!AddValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Sub:
      if (!SubValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Mul:
      if (!MulValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Div:
      if (!DivValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Mod:
      if (!ModValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::Pow:
      if (!PowValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    case JSOp::BitOr: {
      if (!BitOr(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::BitXor: {
      if (!BitXor(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::BitAnd: {
      if (!BitAnd(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Lsh: {
      if (!BitLsh(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Rsh: {
      if (!BitRsh(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    case JSOp::Ursh: {
      if (!UrshValues(cx, &lhsCopy, &rhsCopy, ret)) {
        return false;
      }
      break;
    }
    default:
      MOZ_CRASH("Unhandled binary arith op");
  }

  TryAttachIonStub<BinaryArithIRGenerator>(cx, ic, ionScript, op, lhs, rhs,
                                           ret);

  return true;
}

/* static */
bool IonCompareIC::update(JSContext* cx, HandleScript outerScript,
                          IonCompareIC* ic, HandleValue lhs, HandleValue rhs,
                          bool* res) {
  IonScript* ionScript = outerScript->ionScript();
  RootedScript script(cx, ic->script());
  jsbytecode* pc = ic->pc();
  JSOp op = JSOp(*pc);

  // Don't pass lhs/rhs directly, we need the original values when
  // generating stubs.
  RootedValue lhsCopy(cx, lhs);
  RootedValue rhsCopy(cx, rhs);

  // Perform the compare operation.
  switch (op) {
    case JSOp::Lt:
      if (!LessThan(cx, &lhsCopy, &rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::Le:
      if (!LessThanOrEqual(cx, &lhsCopy, &rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::Gt:
      if (!GreaterThan(cx, &lhsCopy, &rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::Ge:
      if (!GreaterThanOrEqual(cx, &lhsCopy, &rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::Eq:
      if (!js::LooselyEqual(cx, lhsCopy, rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::Ne:
      if (!js::LooselyEqual(cx, lhsCopy, rhsCopy, res)) {
        return false;
      }
      *res = !*res;
      break;
    case JSOp::StrictEq:
      if (!js::StrictlyEqual(cx, lhsCopy, rhsCopy, res)) {
        return false;
      }
      break;
    case JSOp::StrictNe:
      if (!js::StrictlyEqual(cx, lhsCopy, rhsCopy, res)) {
        return false;
      }
      *res = !*res;
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unhandled ion compare op");
      return false;
  }

  TryAttachIonStub<CompareIRGenerator>(cx, ic, ionScript, op, lhs, rhs);

  return true;
}

uint8_t* IonICStub::stubDataStart() {
  return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

void IonIC::attachStub(IonICStub* newStub, JitCode* code) {
  MOZ_ASSERT(newStub);
  MOZ_ASSERT(code);

  if (firstStub_) {
    newStub->setNext(firstStub_, codeRaw_);
  }
  firstStub_ = newStub;
  codeRaw_ = code->raw();

  state_.trackAttached();
}
