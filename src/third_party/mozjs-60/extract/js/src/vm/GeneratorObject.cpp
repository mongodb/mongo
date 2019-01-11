/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GeneratorObject.h"

#include "vm/JSObject.h"

#include "vm/ArrayObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

JSObject*
GeneratorObject::create(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT(frame.script()->isGenerator() || frame.script()->isAsync());
    MOZ_ASSERT(frame.script()->nfixed() == 0);

    Rooted<GlobalObject*> global(cx, cx->global());

    RootedValue pval(cx);
    RootedObject fun(cx, frame.callee());
    // FIXME: This would be faster if we could avoid doing a lookup to get
    // the prototype for the instance.  Bug 906600.
    if (!GetProperty(cx, fun, fun, cx->names().prototype, &pval))
        return nullptr;
    RootedObject proto(cx, pval.isObject() ? &pval.toObject() : nullptr);
    if (!proto) {
        proto = GlobalObject::getOrCreateGeneratorObjectPrototype(cx, global);
        if (!proto)
            return nullptr;
    }
    RootedNativeObject obj(cx,
                           NewNativeObjectWithGivenProto(cx, &GeneratorObject::class_, proto));
    if (!obj)
        return nullptr;

    GeneratorObject* genObj = &obj->as<GeneratorObject>();
    genObj->setCallee(*frame.callee());
    genObj->setNewTarget(frame.newTarget());
    genObj->setEnvironmentChain(*frame.environmentChain());
    if (frame.script()->needsArgsObj())
        genObj->setArgsObj(frame.argsObj());
    genObj->clearExpressionStack();

    return obj;
}

bool
GeneratorObject::suspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame, jsbytecode* pc,
                         Value* vp, unsigned nvalues)
{
    MOZ_ASSERT(*pc == JSOP_INITIALYIELD || *pc == JSOP_YIELD || *pc == JSOP_AWAIT);

    Rooted<GeneratorObject*> genObj(cx, &obj->as<GeneratorObject>());
    MOZ_ASSERT(!genObj->hasExpressionStack() || genObj->isExpressionStackEmpty());
    MOZ_ASSERT_IF(*pc == JSOP_AWAIT, genObj->callee().isAsync());
    MOZ_ASSERT_IF(*pc == JSOP_YIELD, genObj->callee().isGenerator());

    ArrayObject* stack = nullptr;
    if (nvalues > 0) {
        do {
            if (genObj->hasExpressionStack()) {
                MOZ_ASSERT(genObj->expressionStack().getDenseInitializedLength() == 0);
                auto result = genObj->expressionStack().setOrExtendDenseElements(
                    cx, 0, vp, nvalues, ShouldUpdateTypes::DontUpdate);
                if (result == DenseElementResult::Success) {
                    MOZ_ASSERT(genObj->expressionStack().getDenseInitializedLength() == nvalues);
                    break;
                }
                if (result == DenseElementResult::Failure)
                    return false;
            }

            stack = NewDenseCopiedArray(cx, nvalues, vp);
            if (!stack)
                return false;
        } while (false);
    }

    uint32_t yieldAndAwaitIndex = GET_UINT24(pc);
    genObj->setYieldAndAwaitIndex(yieldAndAwaitIndex);
    genObj->setEnvironmentChain(*frame.environmentChain());
    if (stack)
        genObj->setExpressionStack(*stack);

    return true;
}

void
GeneratorObject::finalSuspend(HandleObject obj)
{
    GeneratorObject* genObj = &obj->as<GeneratorObject>();
    MOZ_ASSERT(genObj->isRunning() || genObj->isClosing());
    genObj->setClosed();
}

void
js::SetGeneratorClosed(JSContext* cx, AbstractFramePtr frame)
{
    CallObject& callObj = frame.callObj();

    // Get the generator object stored on the scope chain and close it.
    Shape* shape = callObj.lookup(cx, cx->names().dotGenerator);
    GeneratorObject& genObj = callObj.getSlot(shape->slot()).toObject().as<GeneratorObject>();
    genObj.setClosed();
}

bool
js::GeneratorThrowOrReturn(JSContext* cx, AbstractFramePtr frame, Handle<GeneratorObject*> genObj,
                           HandleValue arg, uint32_t resumeKind)
{
    if (resumeKind == GeneratorObject::THROW) {
        cx->setPendingException(arg);
        genObj->setRunning();
    } else {
        MOZ_ASSERT(resumeKind == GeneratorObject::RETURN);

        MOZ_ASSERT(arg.isObject());
        frame.setReturnValue(arg);

        cx->setPendingException(MagicValue(JS_GENERATOR_CLOSING));
        genObj->setClosing();
    }
    return false;
}

bool
GeneratorObject::resume(JSContext* cx, InterpreterActivation& activation,
                        HandleObject obj, HandleValue arg, GeneratorObject::ResumeKind resumeKind)
{
    Rooted<GeneratorObject*> genObj(cx, &obj->as<GeneratorObject>());
    MOZ_ASSERT(genObj->isSuspended());

    RootedFunction callee(cx, &genObj->callee());
    RootedValue newTarget(cx, genObj->newTarget());
    RootedObject envChain(cx, &genObj->environmentChain());
    if (!activation.resumeGeneratorFrame(callee, newTarget, envChain))
        return false;
    activation.regs().fp()->setResumedGenerator();

    if (genObj->hasArgsObj())
        activation.regs().fp()->initArgsObj(genObj->argsObj());

    if (genObj->hasExpressionStack() && !genObj->isExpressionStackEmpty()) {
        uint32_t len = genObj->expressionStack().getDenseInitializedLength();
        MOZ_ASSERT(activation.regs().spForStackDepth(len));
        const Value* src = genObj->expressionStack().getDenseElements();
        mozilla::PodCopy(activation.regs().sp, src, len);
        activation.regs().sp += len;
        genObj->expressionStack().setDenseInitializedLength(0);
    }

    JSScript* script = callee->nonLazyScript();
    uint32_t offset = script->yieldAndAwaitOffsets()[genObj->yieldAndAwaitIndex()];
    activation.regs().pc = script->offsetToPC(offset);

    // Always push on a value, even if we are raising an exception. In the
    // exception case, the stack needs to have something on it so that exception
    // handling doesn't skip the catch blocks. See TryNoteIter::settle.
    activation.regs().sp++;
    MOZ_ASSERT(activation.regs().spForStackDepth(activation.regs().stackDepth()));
    activation.regs().sp[-1] = arg;

    switch (resumeKind) {
      case NEXT:
        genObj->setRunning();
        return true;

      case THROW:
      case RETURN:
        return GeneratorThrowOrReturn(cx, activation.regs().fp(), genObj, arg, resumeKind);

      default:
        MOZ_CRASH("bad resumeKind");
    }
}

const Class GeneratorObject::class_ = {
    "Generator",
    JSCLASS_HAS_RESERVED_SLOTS(GeneratorObject::RESERVED_SLOTS)
};

static const JSFunctionSpec generator_methods[] = {
    JS_SELF_HOSTED_FN("next", "GeneratorNext", 1, 0),
    JS_SELF_HOSTED_FN("throw", "GeneratorThrow", 1, 0),
    JS_SELF_HOSTED_FN("return", "GeneratorReturn", 1, 0),
    JS_FS_END
};

JSObject*
js::NewSingletonObjectWithFunctionPrototype(JSContext* cx, Handle<GlobalObject*> global)
{
    RootedObject proto(cx, GlobalObject::getOrCreateFunctionPrototype(cx, global));
    if (!proto)
        return nullptr;
    return NewObjectWithGivenProto<PlainObject>(cx, proto, SingletonObject);
}

/* static */ bool
GlobalObject::initGenerators(JSContext* cx, Handle<GlobalObject*> global)
{
    if (global->getReservedSlot(GENERATOR_OBJECT_PROTO).isObject())
        return true;

    RootedObject iteratorProto(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!iteratorProto)
        return false;

    RootedObject genObjectProto(cx, GlobalObject::createBlankPrototypeInheriting(cx, global,
                                                                                 &PlainObject::class_,
                                                                                 iteratorProto));
    if (!genObjectProto)
        return false;
    if (!DefinePropertiesAndFunctions(cx, genObjectProto, nullptr, generator_methods) ||
        !DefineToStringTag(cx, genObjectProto, cx->names().Generator))
    {
        return false;
    }

    RootedObject genFunctionProto(cx, NewSingletonObjectWithFunctionPrototype(cx, global));
    if (!genFunctionProto || !JSObject::setDelegate(cx, genFunctionProto))
        return false;
    if (!LinkConstructorAndPrototype(cx, genFunctionProto, genObjectProto, JSPROP_READONLY,
                                     JSPROP_READONLY) ||
        !DefineToStringTag(cx, genFunctionProto, cx->names().GeneratorFunction))
    {
        return false;
    }

    RootedValue function(cx, global->getConstructor(JSProto_Function));
    if (!function.toObjectOrNull())
        return false;
    RootedObject proto(cx, &function.toObject());
    RootedAtom name(cx, cx->names().GeneratorFunction);
    RootedObject genFunction(cx, NewFunctionWithProto(cx, Generator, 1,
                                                      JSFunction::NATIVE_CTOR, nullptr, name,
                                                      proto, gc::AllocKind::FUNCTION,
                                                      SingletonObject));
    if (!genFunction)
        return false;
    if (!LinkConstructorAndPrototype(cx, genFunction, genFunctionProto,
                                     JSPROP_PERMANENT | JSPROP_READONLY, JSPROP_READONLY))
    {
        return false;
    }

    global->setReservedSlot(GENERATOR_OBJECT_PROTO, ObjectValue(*genObjectProto));
    global->setReservedSlot(GENERATOR_FUNCTION, ObjectValue(*genFunction));
    global->setReservedSlot(GENERATOR_FUNCTION_PROTO, ObjectValue(*genFunctionProto));
    return true;
}

MOZ_MUST_USE bool
js::CheckGeneratorResumptionValue(JSContext* cx, HandleValue v)
{
    // yield/return value should be an Object.
    if (!v.isObject())
        return false;

    JSObject* obj = &v.toObject();

    // It should have `done` data property with boolean value.
    Value doneVal;
    if (!GetPropertyPure(cx, obj, NameToId(cx->names().done), &doneVal))
        return false;
    if (!doneVal.isBoolean())
        return false;

    // It should have `value` data property, but the type doesn't matter
    JSObject* ignored;
    PropertyResult prop;
    if (!LookupPropertyPure(cx, obj, NameToId(cx->names().value), &ignored, &prop))
        return false;
    if (!prop)
        return false;
    if (!prop.isNativeProperty())
        return false;
    if (!prop.shape()->hasDefaultGetter())
        return false;

    return true;
}

bool
GeneratorObject::isAfterYield()
{
    return isAfterYieldOrAwait(JSOP_YIELD);
}

bool
GeneratorObject::isAfterAwait()
{
    return isAfterYieldOrAwait(JSOP_AWAIT);
}

bool
GeneratorObject::isAfterYieldOrAwait(JSOp op)
{
    if (isClosed() || isClosing() || isRunning())
        return false;

    JSScript* script = callee().nonLazyScript();
    jsbytecode* code = script->code();
    uint32_t nextOffset = script->yieldAndAwaitOffsets()[yieldAndAwaitIndex()];
    if (code[nextOffset] != JSOP_DEBUGAFTERYIELD)
        return false;

    uint32_t offset = nextOffset - JSOP_YIELD_LENGTH;
    MOZ_ASSERT(code[offset] == JSOP_INITIALYIELD || code[offset] == JSOP_YIELD ||
               code[offset] == JSOP_AWAIT);

    return code[offset] == op;
}
