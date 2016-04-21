/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GeneratorObject.h"

#include "jsatominlines.h"
#include "jsscriptinlines.h"

#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

JSObject*
GeneratorObject::create(JSContext* cx, AbstractFramePtr frame)
{
    MOZ_ASSERT(frame.script()->isGenerator());
    MOZ_ASSERT(frame.script()->nfixed() == 0);

    Rooted<GlobalObject*> global(cx, cx->global());
    RootedNativeObject obj(cx);
    if (frame.script()->isStarGenerator()) {
        RootedValue pval(cx);
        RootedObject fun(cx, frame.fun());
        // FIXME: This would be faster if we could avoid doing a lookup to get
        // the prototype for the instance.  Bug 906600.
        if (!GetProperty(cx, fun, fun, cx->names().prototype, &pval))
            return nullptr;
        RootedObject proto(cx, pval.isObject() ? &pval.toObject() : nullptr);
        if (!proto) {
            proto = GlobalObject::getOrCreateStarGeneratorObjectPrototype(cx, global);
            if (!proto)
                return nullptr;
        }
        obj = NewNativeObjectWithGivenProto(cx, &StarGeneratorObject::class_, proto);
    } else {
        MOZ_ASSERT(frame.script()->isLegacyGenerator());
        RootedObject proto(cx, GlobalObject::getOrCreateLegacyGeneratorObjectPrototype(cx, global));
        if (!proto)
            return nullptr;
        obj = NewNativeObjectWithGivenProto(cx, &LegacyGeneratorObject::class_, proto);
    }
    if (!obj)
        return nullptr;

    GeneratorObject* genObj = &obj->as<GeneratorObject>();
    genObj->setCallee(*frame.callee());
    genObj->setNewTarget(frame.newTarget());
    genObj->setScopeChain(*frame.scopeChain());
    if (frame.script()->needsArgsObj())
        genObj->setArgsObj(frame.argsObj());
    genObj->clearExpressionStack();

    return obj;
}

bool
GeneratorObject::suspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame, jsbytecode* pc,
                         Value* vp, unsigned nvalues)
{
    MOZ_ASSERT(*pc == JSOP_INITIALYIELD || *pc == JSOP_YIELD);

    Rooted<GeneratorObject*> genObj(cx, &obj->as<GeneratorObject>());
    MOZ_ASSERT(!genObj->hasExpressionStack());

    if (*pc == JSOP_YIELD && genObj->isClosing() && genObj->is<LegacyGeneratorObject>()) {
        RootedValue val(cx, ObjectValue(*frame.callee()));
        ReportValueError(cx, JSMSG_BAD_GENERATOR_YIELD, JSDVG_IGNORE_STACK, val, nullptr);
        return false;
    }

    uint32_t yieldIndex = GET_UINT24(pc);
    genObj->setYieldIndex(yieldIndex);
    genObj->setScopeChain(*frame.scopeChain());

    if (nvalues) {
        ArrayObject* stack = NewDenseCopiedArray(cx, nvalues, vp);
        if (!stack)
            return false;
        genObj->setExpressionStack(*stack);
    }

    return true;
}

bool
GeneratorObject::finalSuspend(JSContext* cx, HandleObject obj)
{
    Rooted<GeneratorObject*> genObj(cx, &obj->as<GeneratorObject>());
    MOZ_ASSERT(genObj->isRunning() || genObj->isClosing());

    bool closing = genObj->isClosing();
    genObj->setClosed();

    if (genObj->is<LegacyGeneratorObject>() && !closing)
        return ThrowStopIteration(cx);

    return true;
}

void
js::SetReturnValueForClosingGenerator(JSContext* cx, AbstractFramePtr frame)
{
    CallObject& callObj = frame.callObj();

    // Get the generator object stored on the scope chain and close it.
    Shape* shape = callObj.lookup(cx, cx->names().dotGenerator);
    GeneratorObject& genObj = callObj.getSlot(shape->slot()).toObject().as<GeneratorObject>();
    genObj.setClosed();

    // Return value is already set in GeneratorThrowOrClose.
    if (genObj.is<StarGeneratorObject>())
        return;

    // Legacy generator .close() always returns |undefined|.
    MOZ_ASSERT(genObj.is<LegacyGeneratorObject>());
    frame.setReturnValue(UndefinedValue());
}

bool
js::GeneratorThrowOrClose(JSContext* cx, AbstractFramePtr frame, Handle<GeneratorObject*> genObj,
                          HandleValue arg, uint32_t resumeKind)
{
    if (resumeKind == GeneratorObject::THROW) {
        cx->setPendingException(arg);
        genObj->setRunning();
    } else {
        MOZ_ASSERT(resumeKind == GeneratorObject::CLOSE);

        if (genObj->is<StarGeneratorObject>()) {
            MOZ_ASSERT(arg.isObject());
            frame.setReturnValue(arg);
        } else {
            MOZ_ASSERT(arg.isUndefined());
        }

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
    RootedObject scopeChain(cx, &genObj->scopeChain());
    if (!activation.resumeGeneratorFrame(callee, newTarget, scopeChain))
        return false;
    activation.regs().fp()->setResumedGenerator();

    if (genObj->hasArgsObj())
        activation.regs().fp()->initArgsObj(genObj->argsObj());

    if (genObj->hasExpressionStack()) {
        uint32_t len = genObj->expressionStack().length();
        MOZ_ASSERT(activation.regs().spForStackDepth(len));
        const Value* src = genObj->expressionStack().getDenseElements();
        mozilla::PodCopy(activation.regs().sp, src, len);
        activation.regs().sp += len;
        genObj->clearExpressionStack();
    }

    JSScript* script = callee->nonLazyScript();
    uint32_t offset = script->yieldOffsets()[genObj->yieldIndex()];
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
      case CLOSE:
        return GeneratorThrowOrClose(cx, activation.regs().fp(), genObj, arg, resumeKind);

      default:
        MOZ_CRASH("bad resumeKind");
    }
}

bool
LegacyGeneratorObject::close(JSContext* cx, HandleObject obj)
{
     Rooted<LegacyGeneratorObject*> genObj(cx, &obj->as<LegacyGeneratorObject>());

    // Avoid calling back into JS unless it is necessary.
     if (genObj->isClosed())
        return true;

    RootedValue rval(cx);

    RootedValue closeValue(cx);
    if (!GlobalObject::getIntrinsicValue(cx, cx->global(), cx->names().LegacyGeneratorCloseInternal,
                                         &closeValue))
    {
        return false;
    }
    MOZ_ASSERT(closeValue.isObject());
    MOZ_ASSERT(closeValue.toObject().is<JSFunction>());

    InvokeArgs args(cx);
    if (!args.init(0))
        return false;

    args.setCallee(closeValue);
    args.setThis(ObjectValue(*genObj));

    return Invoke(cx, args);
}

const Class LegacyGeneratorObject::class_ = {
    "Generator",
    JSCLASS_HAS_RESERVED_SLOTS(GeneratorObject::RESERVED_SLOTS)
};

const Class StarGeneratorObject::class_ = {
    "Generator",
    JSCLASS_HAS_RESERVED_SLOTS(GeneratorObject::RESERVED_SLOTS)
};

static const JSFunctionSpec star_generator_methods[] = {
    JS_SELF_HOSTED_FN("next", "StarGeneratorNext", 1, 0),
    JS_SELF_HOSTED_FN("throw", "StarGeneratorThrow", 1, 0),
    JS_SELF_HOSTED_FN("return", "StarGeneratorReturn", 1, 0),
    JS_FS_END
};

#define JSPROP_ROPERM   (JSPROP_READONLY | JSPROP_PERMANENT)

static const JSFunctionSpec legacy_generator_methods[] = {
    JS_SELF_HOSTED_SYM_FN(iterator, "LegacyGeneratorIteratorShim", 0, 0),
    // "send" is an alias for "next".
    JS_SELF_HOSTED_FN("next", "LegacyGeneratorNext", 1, JSPROP_ROPERM),
    JS_SELF_HOSTED_FN("send", "LegacyGeneratorNext", 1, JSPROP_ROPERM),
    JS_SELF_HOSTED_FN("throw", "LegacyGeneratorThrow", 1, JSPROP_ROPERM),
    JS_SELF_HOSTED_FN("close", "LegacyGeneratorClose", 0, JSPROP_ROPERM),
    JS_FS_END
};

#undef JSPROP_ROPERM

static JSObject*
NewSingletonObjectWithObjectPrototype(JSContext* cx, Handle<GlobalObject*> global)
{
    RootedObject proto(cx, global->getOrCreateObjectPrototype(cx));
    if (!proto)
        return nullptr;
    return NewObjectWithGivenProto<PlainObject>(cx, proto, SingletonObject);
}

static JSObject*
NewSingletonObjectWithFunctionPrototype(JSContext* cx, Handle<GlobalObject*> global)
{
    RootedObject proto(cx, global->getOrCreateFunctionPrototype(cx));
    if (!proto)
        return nullptr;
    return NewObjectWithGivenProto<PlainObject>(cx, proto, SingletonObject);
}

/* static */ bool
GlobalObject::initLegacyGeneratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    if (global->getReservedSlot(LEGACY_GENERATOR_OBJECT_PROTO).isObject())
        return true;

    RootedObject proto(cx, NewSingletonObjectWithObjectPrototype(cx, global));
    if (!proto || !proto->setDelegate(cx))
        return false;
    if (!DefinePropertiesAndFunctions(cx, proto, nullptr, legacy_generator_methods))
        return false;

    global->setReservedSlot(LEGACY_GENERATOR_OBJECT_PROTO, ObjectValue(*proto));
    return true;
}

/* static */ bool
GlobalObject::initStarGenerators(JSContext* cx, Handle<GlobalObject*> global)
{
    if (global->getReservedSlot(STAR_GENERATOR_OBJECT_PROTO).isObject())
        return true;

    RootedObject iteratorProto(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!iteratorProto)
        return false;

    RootedObject genObjectProto(cx, global->createBlankPrototypeInheriting(cx,
                                                                           &PlainObject::class_,
                                                                           iteratorProto));
    if (!genObjectProto)
        return false;
    if (!DefinePropertiesAndFunctions(cx, genObjectProto, nullptr, star_generator_methods))
        return false;

    RootedObject genFunctionProto(cx, NewSingletonObjectWithFunctionPrototype(cx, global));
    if (!genFunctionProto || !genFunctionProto->setDelegate(cx))
        return false;
    if (!LinkConstructorAndPrototype(cx, genFunctionProto, genObjectProto))
        return false;

    RootedValue function(cx, global->getConstructor(JSProto_Function));
    if (!function.toObjectOrNull())
        return false;
    RootedObject proto(cx, &function.toObject());
    RootedAtom name(cx, cx->names().GeneratorFunction);
    RootedObject genFunction(cx, NewFunctionWithProto(cx, Generator, 1,
                                                      JSFunction::NATIVE_CTOR, nullptr, name,
                                                      proto));
    if (!genFunction)
        return false;
    if (!LinkConstructorAndPrototype(cx, genFunction, genFunctionProto))
        return false;

    global->setReservedSlot(STAR_GENERATOR_OBJECT_PROTO, ObjectValue(*genObjectProto));
    global->setReservedSlot(STAR_GENERATOR_FUNCTION, ObjectValue(*genFunction));
    global->setReservedSlot(STAR_GENERATOR_FUNCTION_PROTO, ObjectValue(*genFunctionProto));
    return true;
}
