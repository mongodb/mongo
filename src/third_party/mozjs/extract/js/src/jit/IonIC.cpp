/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/IonIC.h"

#include "jit/CacheIRCompiler.h"
#include "jit/Linker.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;
using namespace js::jit;

void
IonIC::updateBaseAddress(JitCode* code, MacroAssembler& masm)
{
    fallbackLabel_.repoint(code, &masm);
    rejoinLabel_.repoint(code, &masm);

    codeRaw_ = fallbackLabel_.raw();
}

Register
IonIC::scratchRegisterForEntryJump()
{
    switch (kind_) {
      case CacheKind::GetProp:
      case CacheKind::GetElem: {
        Register temp = asGetPropertyIC()->maybeTemp();
        if (temp != InvalidReg)
            return temp;
        TypedOrValueRegister output = asGetPropertyIC()->output();
        return output.hasValue() ? output.valueReg().scratchReg() : output.typedReg().gpr();
      }
      case CacheKind::GetPropSuper:
      case CacheKind::GetElemSuper: {
        TypedOrValueRegister output = asGetPropSuperIC()->output();
        return output.valueReg().scratchReg();
      }
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
      case CacheKind::GetIterator:
        return asGetIteratorIC()->temp1();
      case CacheKind::InstanceOf:
        return asInstanceOfIC()->output();
      case CacheKind::Call:
      case CacheKind::Compare:
      case CacheKind::TypeOf:
      case CacheKind::ToBool:
      case CacheKind::GetIntrinsic:
        MOZ_CRASH("Unsupported IC");
    }

    MOZ_CRASH("Invalid kind");
}

void
IonIC::discardStubs(Zone* zone)
{
    if (firstStub_ && zone->needsIncrementalBarrier()) {
        // We are removing edges from IonIC to gcthings. Perform one final trace
        // of the stub for incremental GC, as it must know about those edges.
        trace(zone->barrierTracer());
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
    codeRaw_ = fallbackLabel_.raw();
    state_.trackUnlinkedAllStubs();
}

void
IonIC::reset(Zone* zone)
{
    discardStubs(zone);
    state_.reset();
}

void
IonIC::trace(JSTracer* trc)
{
    if (script_)
        TraceManuallyBarrieredEdge(trc, &script_, "IonIC::script_");

    uint8_t* nextCodeRaw = codeRaw_;
    for (IonICStub* stub = firstStub_; stub; stub = stub->next()) {
        JitCode* code = JitCode::FromExecutable(nextCodeRaw);
        TraceManuallyBarrieredEdge(trc, &code, "ion-ic-code");

        TraceCacheIRStub(trc, stub, stub->stubInfo());

        nextCodeRaw = stub->nextCodeRaw();
    }

    MOZ_ASSERT(nextCodeRaw == fallbackLabel_.raw());
}

/* static */ bool
IonGetPropertyIC::update(JSContext* cx, HandleScript outerScript, IonGetPropertyIC* ic,
                         HandleValue val, HandleValue idVal, MutableHandleValue res)
{
    // Override the return value if we are invalidated (bug 728188).
    IonScript* ionScript = outerScript->ionScript();
    AutoDetectInvalidation adi(cx, res, ionScript);

    // If the IC is idempotent, we will redo the op in the interpreter.
    if (ic->idempotent())
        adi.disable();

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    bool attached = false;
    if (ic->state().canAttachStub()) {
        // IonBuilder calls PropertyReadNeedsTypeBarrier to determine if it
        // needs a type barrier. Unfortunately, PropertyReadNeedsTypeBarrier
        // does not account for getters, so we should only attach a getter
        // stub if we inserted a type barrier.
        jsbytecode* pc = ic->idempotent() ? nullptr : ic->pc();
        bool isTemporarilyUnoptimizable = false;
        GetPropIRGenerator gen(cx, outerScript, pc, ic->kind(), ic->state().mode(),
                               &isTemporarilyUnoptimizable, val, idVal, val,
                               ic->resultFlags());
        if (ic->idempotent() ? gen.tryAttachIdempotentStub() : gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached && !isTemporarilyUnoptimizable)
            ic->state().trackNotAttached();
    }

    if (!attached && ic->idempotent()) {
        // Invalidate the cache if the property was not found, or was found on
        // a non-native object. This ensures:
        // 1) The property read has no observable side-effects.
        // 2) There's no need to dynamically monitor the return type. This would
        //    be complicated since (due to GVN) there can be multiple pc's
        //    associated with a single idempotent cache.
        JitSpew(JitSpew_IonIC, "Invalidating from idempotent cache %s:%zu",
                outerScript->filename(), outerScript->lineno());

        outerScript->setInvalidatedIdempotentCache();

        // Do not re-invalidate if the lookup already caused invalidation.
        if (outerScript->hasIonScript())
            Invalidate(cx, outerScript);

        // We will redo the potentially effectful lookup in Baseline.
        return true;
    }

    if (ic->kind() == CacheKind::GetProp) {
        RootedPropertyName name(cx, idVal.toString()->asAtom().asPropertyName());
        if (!GetProperty(cx, val, name, res))
            return false;
    } else {
        MOZ_ASSERT(ic->kind() == CacheKind::GetElem);
        if (!GetElementOperation(cx, JSOp(*ic->pc()), val, idVal, res))
            return false;
    }

    if (!ic->idempotent()) {
        // Monitor changes to cache entry.
        if (!ic->monitoredResult())
            TypeScript::Monitor(cx, ic->script(), ic->pc(), res);
    }

    return true;
}

/* static */ bool
IonGetPropSuperIC::update(JSContext* cx, HandleScript outerScript, IonGetPropSuperIC* ic,
                          HandleObject obj, HandleValue receiver, HandleValue idVal, MutableHandleValue res)
{
    // Override the return value if we are invalidated (bug 728188).
    IonScript* ionScript = outerScript->ionScript();
    AutoDetectInvalidation adi(cx, res, ionScript);

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    bool attached = false;
    if (ic->state().canAttachStub()) {
        RootedValue val(cx, ObjectValue(*obj));
        bool isTemporarilyUnoptimizable = false;
        GetPropIRGenerator gen(cx, outerScript, ic->pc(), ic->kind(), ic->state().mode(),
                               &isTemporarilyUnoptimizable, val, idVal, receiver,
                               GetPropertyResultFlags::All);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached && !isTemporarilyUnoptimizable)
            ic->state().trackNotAttached();
    }

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, idVal, &id))
        return false;

    if (!GetProperty(cx, obj, receiver, id, res))
        return false;

    // Monitor changes to cache entry.
    TypeScript::Monitor(cx, ic->script(), ic->pc(), res);
    return true;
}

/* static */ bool
IonSetPropertyIC::update(JSContext* cx, HandleScript outerScript, IonSetPropertyIC* ic,
                         HandleObject obj, HandleValue idVal, HandleValue rhs)
{
    RootedShape oldShape(cx);
    RootedObjectGroup oldGroup(cx);
    IonScript* ionScript = outerScript->ionScript();

    bool attached = false;
    bool isTemporarilyUnoptimizable = false;

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        oldShape = obj->maybeShape();
        oldGroup = JSObject::getGroup(cx, obj);
        if (!oldGroup)
            return false;
        if (obj->is<UnboxedPlainObject>()) {
            MOZ_ASSERT(!oldShape);
            if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando())
                oldShape = expando->lastProperty();
        }

        RootedValue objv(cx, ObjectValue(*obj));
        RootedScript script(cx, ic->script());
        jsbytecode* pc = ic->pc();
        SetPropIRGenerator gen(cx, script, pc, ic->kind(), ic->state().mode(),
                               &isTemporarilyUnoptimizable,
                               objv, idVal, rhs, ic->needsTypeBarrier(), ic->guardHoles());
        if (gen.tryAttachStub()) {
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached,
                                  gen.typeCheckInfo());
        }
    }

    jsbytecode* pc = ic->pc();
    if (ic->kind() == CacheKind::SetElem) {
        if (*pc == JSOP_INITELEM_INC) {
            if (!InitArrayElemOperation(cx, pc, obj, idVal.toInt32(), rhs))
                return false;
        } else if (IsPropertyInitOp(JSOp(*pc))) {
            if (!InitElemOperation(cx, pc, obj, idVal, rhs))
                return false;
        } else {
            MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));
            if (!SetObjectElement(cx, obj, idVal, rhs, ic->strict()))
                return false;
        }
    } else {
        MOZ_ASSERT(ic->kind() == CacheKind::SetProp);

        if (*pc == JSOP_INITGLEXICAL) {
            RootedScript script(cx, ic->script());
            MOZ_ASSERT(!script->hasNonSyntacticScope());
            InitGlobalLexicalOperation(cx, &cx->global()->lexicalEnvironment(), script, pc, rhs);
        } else if (IsPropertyInitOp(JSOp(*pc))) {
            // This might be a JSOP_INITELEM op with a constant string id. We
            // can't call InitPropertyOperation here as that function is
            // specialized for JSOP_INIT*PROP (it does not support arbitrary
            // objects that might show up here).
            if (!InitElemOperation(cx, pc, obj, idVal, rhs))
                return false;
        } else {
            MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));
            RootedPropertyName name(cx, idVal.toString()->asAtom().asPropertyName());
            if (!SetProperty(cx, obj, name, rhs, ic->strict(), pc))
                return false;
        }
    }

    if (attached)
        return true;

    // The SetProperty call might have entered this IC recursively, so try
    // to transition.
    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        RootedValue objv(cx, ObjectValue(*obj));
        RootedScript script(cx, ic->script());
        jsbytecode* pc = ic->pc();
        SetPropIRGenerator gen(cx, script, pc, ic->kind(), ic->state().mode(),
                               &isTemporarilyUnoptimizable,
                               objv, idVal, rhs, ic->needsTypeBarrier(), ic->guardHoles());
        if (gen.tryAttachAddSlotStub(oldGroup, oldShape)) {
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached,
                                  gen.typeCheckInfo());
        } else {
            gen.trackAttached(nullptr);
        }

        if (!attached && !isTemporarilyUnoptimizable)
            ic->state().trackNotAttached();
    }

    return true;
}

/* static */ bool
IonGetNameIC::update(JSContext* cx, HandleScript outerScript, IonGetNameIC* ic,
                     HandleObject envChain, MutableHandleValue res)
{
    IonScript* ionScript = outerScript->ionScript();
    jsbytecode* pc = ic->pc();
    RootedPropertyName name(cx, ic->script()->getName(pc));

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        GetNameIRGenerator gen(cx, script, pc, ic->state().mode(), envChain, name);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    RootedObject obj(cx);
    RootedObject holder(cx);
    Rooted<PropertyResult> prop(cx);
    if (!LookupName(cx, name, envChain, &obj, &holder, &prop))
        return false;

    if (*GetNextPc(pc) == JSOP_TYPEOF) {
        if (!FetchName<GetNameMode::TypeOf>(cx, obj, holder, name, prop, res))
            return false;
    } else {
        if (!FetchName<GetNameMode::Normal>(cx, obj, holder, name, prop, res))
            return false;
    }

    // No need to call TypeScript::Monitor, IonBuilder always inserts a type
    // barrier after GetName ICs.

    return true;
}

/* static */ JSObject*
IonBindNameIC::update(JSContext* cx, HandleScript outerScript, IonBindNameIC* ic,
                      HandleObject envChain)
{
    IonScript* ionScript = outerScript->ionScript();
    jsbytecode* pc = ic->pc();
    RootedPropertyName name(cx, ic->script()->getName(pc));

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        BindNameIRGenerator gen(cx, script, pc, ic->state().mode(), envChain, name);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    RootedObject holder(cx);
    if (!LookupNameUnqualified(cx, name, envChain, &holder))
        return nullptr;

    return holder;
}

/* static */ JSObject*
IonGetIteratorIC::update(JSContext* cx, HandleScript outerScript, IonGetIteratorIC* ic,
                         HandleValue value)
{
    IonScript* ionScript = outerScript->ionScript();

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        GetIteratorIRGenerator gen(cx, script, ic->pc(), ic->state().mode(), value);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    return ValueToIterator(cx, value);
}

/* static */ bool
IonHasOwnIC::update(JSContext* cx, HandleScript outerScript, IonHasOwnIC* ic,
                    HandleValue val, HandleValue idVal, int32_t* res)
{
    IonScript* ionScript = outerScript->ionScript();

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    jsbytecode* pc = ic->pc();

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        HasPropIRGenerator gen(cx, script, pc, CacheKind::HasOwn, ic->state().mode(), idVal, val);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    bool found;
    if (!HasOwnProperty(cx, val, idVal, &found))
        return false;

    *res = found;
    return true;
}

/* static */ bool
IonInIC::update(JSContext* cx, HandleScript outerScript, IonInIC* ic,
                HandleValue key, HandleObject obj, bool* res)
{
    IonScript* ionScript = outerScript->ionScript();

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        RootedValue objV(cx, ObjectValue(*obj));
        jsbytecode* pc = ic->pc();
        HasPropIRGenerator gen(cx, script, pc, CacheKind::In, ic->state().mode(), key, objV);
        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    return OperatorIn(cx, key, obj, res);
}
/* static */ bool
IonInstanceOfIC::update(JSContext* cx, HandleScript outerScript, IonInstanceOfIC* ic,
                        HandleValue lhs, HandleObject rhs, bool* res)
{
    IonScript* ionScript = outerScript->ionScript();

    if (ic->state().maybeTransition())
        ic->discardStubs(cx->zone());

    if (ic->state().canAttachStub()) {
        bool attached = false;
        RootedScript script(cx, ic->script());
        jsbytecode* pc = ic->pc();

        InstanceOfIRGenerator gen(cx, script, pc, ic->state().mode(),
                                  lhs, rhs);

        if (gen.tryAttachStub())
            ic->attachCacheIRStub(cx, gen.writerRef(), gen.cacheKind(), ionScript, &attached);

        if (!attached)
            ic->state().trackNotAttached();
    }

    return HasInstance(cx, rhs, lhs, res);
}

uint8_t*
IonICStub::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

void
IonIC::attachStub(IonICStub* newStub, JitCode* code)
{
    MOZ_ASSERT(newStub);
    MOZ_ASSERT(code);

    if (firstStub_) {
        IonICStub* last = firstStub_;
        while (IonICStub* next = last->next())
            last = next;
        last->setNext(newStub, code);
    } else {
        firstStub_ = newStub;
        codeRaw_ = code->raw();
    }

    state_.trackAttached();
}
