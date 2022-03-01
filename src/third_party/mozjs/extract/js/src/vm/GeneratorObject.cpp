/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GeneratorObject.h"

#include "frontend/ParserAtom.h"
#ifdef DEBUG
#  include "js/friend/DumpFunctions.h"  // js::DumpObject, js::DumpValue
#endif
#include "js/PropertySpec.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/GlobalObject.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject

#include "debugger/DebugAPI-inl.h"
#include "vm/ArrayObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

AbstractGeneratorObject* AbstractGeneratorObject::create(
    JSContext* cx, HandleFunction callee, HandleScript script,
    HandleObject environmentChain, Handle<ArgumentsObject*> argsObject) {
  Rooted<AbstractGeneratorObject*> genObj(cx);
  if (!callee->isAsync()) {
    genObj = GeneratorObject::create(cx, callee);
  } else if (callee->isGenerator()) {
    genObj = AsyncGeneratorObject::create(cx, callee);
  } else {
    genObj = AsyncFunctionGeneratorObject::create(cx, callee);
  }
  if (!genObj) {
    return nullptr;
  }

  genObj->setCallee(*callee);
  genObj->setEnvironmentChain(*environmentChain);
  if (argsObject) {
    genObj->setArgsObj(*argsObject.get());
  }

  ArrayObject* stack = NewDenseFullyAllocatedArray(cx, script->nslots());
  if (!stack) {
    return nullptr;
  }

  genObj->setStackStorage(*stack);

  // Note: This assumes that a Warp frame cannot be the target of
  //       the debugger, as we do not call OnNewGenerator.
  return genObj;
}

JSObject* AbstractGeneratorObject::createFromFrame(JSContext* cx,
                                                   AbstractFramePtr frame) {
  MOZ_ASSERT(frame.isGeneratorFrame());
  MOZ_ASSERT(!frame.isConstructing());

  if (frame.isModuleFrame()) {
    return createModuleGenerator(cx, frame);
  }

  RootedFunction fun(cx, frame.callee());
  Rooted<ArgumentsObject*> maybeArgs(
      cx, frame.script()->needsArgsObj() ? &frame.argsObj() : nullptr);
  RootedObject environmentChain(cx, frame.environmentChain());

  RootedScript script(cx, frame.script());
  Rooted<AbstractGeneratorObject*> genObj(
      cx, AbstractGeneratorObject::create(cx, fun, script, environmentChain,
                                          maybeArgs));
  if (!genObj) {
    return nullptr;
  }

  if (!DebugAPI::onNewGenerator(cx, frame, genObj)) {
    return nullptr;
  }

  return genObj;
}

JSObject* AbstractGeneratorObject::createModuleGenerator(
    JSContext* cx, AbstractFramePtr frame) {
  Rooted<ModuleObject*> module(cx, frame.script()->module());
  Rooted<AbstractGeneratorObject*> genObj(cx);
  genObj = AsyncFunctionGeneratorObject::create(cx, module);
  if (!genObj) {
    return nullptr;
  }

  // Create a handler function to wrap the module's script. This way
  // we can access it later and restore the state.
  HandlePropertyName funName = cx->names().empty;
  RootedFunction handlerFun(
      cx, NewFunctionWithProto(cx, nullptr, 0,
                               FunctionFlags::INTERPRETED_GENERATOR_OR_ASYNC,
                               nullptr, funName, nullptr,
                               gc::AllocKind::FUNCTION, GenericObject));
  if (!handlerFun) {
    return nullptr;
  }
  handlerFun->initScript(module->script());

  genObj->setCallee(*handlerFun);
  genObj->setEnvironmentChain(*frame.environmentChain());

  ArrayObject* stack =
      NewDenseFullyAllocatedArray(cx, module->script()->nslots());
  if (!stack) {
    return nullptr;
  }

  genObj->setStackStorage(*stack);

  if (!DebugAPI::onNewGenerator(cx, frame, genObj)) {
    return nullptr;
  }

  return genObj;
}

void AbstractGeneratorObject::trace(JSTracer* trc) {
  DebugAPI::traceGeneratorFrame(trc, this);
}

bool AbstractGeneratorObject::suspend(JSContext* cx, HandleObject obj,
                                      AbstractFramePtr frame, jsbytecode* pc,
                                      unsigned nvalues) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::InitialYield || JSOp(*pc) == JSOp::Yield ||
             JSOp(*pc) == JSOp::Await);

  auto genObj = obj.as<AbstractGeneratorObject>();
  MOZ_ASSERT(!genObj->hasStackStorage() || genObj->isStackStorageEmpty());
  MOZ_ASSERT_IF(JSOp(*pc) == JSOp::Await, genObj->callee().isAsync());
  MOZ_ASSERT_IF(JSOp(*pc) == JSOp::Yield, genObj->callee().isGenerator());

  if (nvalues > 0) {
    ArrayObject* stack = nullptr;
    MOZ_ASSERT(genObj->hasStackStorage());
    stack = &genObj->stackStorage();
    MOZ_ASSERT(stack->getDenseCapacity() >= nvalues);
    if (!frame.saveGeneratorSlots(cx, nvalues, stack)) {
      return false;
    }
  }

  genObj->setResumeIndex(pc);
  genObj->setEnvironmentChain(*frame.environmentChain());
  return true;
}

#ifdef DEBUG
void AbstractGeneratorObject::dump() const {
  fprintf(stderr, "(AbstractGeneratorObject*) %p {\n", (void*)this);
  fprintf(stderr, "  callee: (JSFunction*) %p,\n", (void*)&callee());
  fprintf(stderr, "  environmentChain: (JSObject*) %p,\n",
          (void*)&environmentChain());
  if (hasArgsObj()) {
    fprintf(stderr, "  argsObj: Some((ArgumentsObject*) %p),\n",
            (void*)&argsObj());
  } else {
    fprintf(stderr, "  argsObj: None,\n");
  }
  if (hasStackStorage()) {
    fprintf(stderr, "  stackStorage: Some(ArrayObject {\n");
    ArrayObject& stack = stackStorage();
    uint32_t denseLen = uint32_t(stack.getDenseInitializedLength());
    fprintf(stderr, "    denseInitializedLength: %u\n,", denseLen);
    uint32_t len = stack.length();
    fprintf(stderr, "    length: %u\n,", len);
    fprintf(stderr, "    data: [\n");
    const Value* elements = getDenseElements();
    for (uint32_t i = 0; i < std::max(len, denseLen); i++) {
      fprintf(stderr, "      [%u]: ", i);
      js::DumpValue(elements[i]);
    }
    fprintf(stderr, "    ],\n");
    fprintf(stderr, "  }),\n");
  } else {
    fprintf(stderr, "  stackStorage: None\n");
  }
  if (isSuspended()) {
    fprintf(stderr, "  resumeIndex: Some(%u),\n", resumeIndex());
  } else {
    fprintf(stderr, "  resumeIndex: None, /* (not suspended) */\n");
  }
  fprintf(stderr, "}\n");
}
#endif

void AbstractGeneratorObject::finalSuspend(HandleObject obj) {
  auto* genObj = &obj->as<AbstractGeneratorObject>();
  MOZ_ASSERT(genObj->isRunning());
  genObj->setClosed();
}

static AbstractGeneratorObject* GetGeneratorObjectForCall(JSContext* cx,
                                                          CallObject& callObj) {
  // The ".generator" binding is always present and always "aliased".
  mozilla::Maybe<PropertyInfo> prop =
      callObj.lookup(cx, cx->names().dotGenerator);
  if (prop.isNothing()) {
    return nullptr;
  }
  Value genValue = callObj.getSlot(prop->slot());

  // If the `Generator; SetAliasedVar ".generator"; InitialYield` bytecode
  // sequence has not run yet, genValue is undefined.
  return genValue.isObject()
             ? &genValue.toObject().as<AbstractGeneratorObject>()
             : nullptr;
}

AbstractGeneratorObject* js::GetGeneratorObjectForFrame(
    JSContext* cx, AbstractFramePtr frame) {
  cx->check(frame);
  MOZ_ASSERT(frame.isGeneratorFrame());

  if (frame.isModuleFrame()) {
    ModuleEnvironmentObject* moduleEnv =
        frame.script()->module()->environment();
    mozilla::Maybe<PropertyInfo> prop =
        moduleEnv->lookup(cx, cx->names().dotGenerator);
    Value genValue = moduleEnv->getSlot(prop->slot());
    return genValue.isObject()
               ? &genValue.toObject().as<AbstractGeneratorObject>()
               : nullptr;
  }
  if (!frame.hasInitialEnvironment()) {
    return nullptr;
  }

  return GetGeneratorObjectForCall(cx, frame.callObj());
}

AbstractGeneratorObject* js::GetGeneratorObjectForEnvironment(
    JSContext* cx, HandleObject env) {
  auto* call = CallObject::find(env);
  return call ? GetGeneratorObjectForCall(cx, *call) : nullptr;
}

bool js::GeneratorThrowOrReturn(JSContext* cx, AbstractFramePtr frame,
                                Handle<AbstractGeneratorObject*> genObj,
                                HandleValue arg,
                                GeneratorResumeKind resumeKind) {
  MOZ_ASSERT(genObj->isRunning());
  if (resumeKind == GeneratorResumeKind::Throw) {
    cx->setPendingExceptionAndCaptureStack(arg);
  } else {
    MOZ_ASSERT(resumeKind == GeneratorResumeKind::Return);

    MOZ_ASSERT_IF(genObj->is<GeneratorObject>(), arg.isObject());
    frame.setReturnValue(arg);

    RootedValue closing(cx, MagicValue(JS_GENERATOR_CLOSING));
    cx->setPendingException(closing, nullptr);
  }
  return false;
}

bool AbstractGeneratorObject::resume(JSContext* cx,
                                     InterpreterActivation& activation,
                                     Handle<AbstractGeneratorObject*> genObj,
                                     HandleValue arg, HandleValue resumeKind) {
  MOZ_ASSERT(genObj->isSuspended());

  RootedFunction callee(cx, &genObj->callee());
  RootedObject envChain(cx, &genObj->environmentChain());
  if (!activation.resumeGeneratorFrame(callee, envChain)) {
    return false;
  }
  activation.regs().fp()->setResumedGenerator();

  if (genObj->hasArgsObj()) {
    activation.regs().fp()->initArgsObj(genObj->argsObj());
  }

  if (genObj->hasStackStorage() && !genObj->isStackStorageEmpty()) {
    JSScript* script = activation.regs().fp()->script();
    ArrayObject* storage = &genObj->stackStorage();
    uint32_t len = storage->getDenseInitializedLength();
    activation.regs().fp()->restoreGeneratorSlots(storage);
    activation.regs().sp += len - script->nfixed();
    storage->setDenseInitializedLength(0);
  }

  JSScript* script = callee->nonLazyScript();
  uint32_t offset = script->resumeOffsets()[genObj->resumeIndex()];
  activation.regs().pc = script->offsetToPC(offset);

  // Push arg, generator, resumeKind Values on the generator's stack.
  activation.regs().sp += 3;
  MOZ_ASSERT(activation.regs().spForStackDepth(activation.regs().stackDepth()));
  activation.regs().sp[-3] = arg;
  activation.regs().sp[-2] = ObjectValue(*genObj);
  activation.regs().sp[-1] = resumeKind;

  genObj->setRunning();
  return true;
}

GeneratorObject* GeneratorObject::create(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(fun->isGenerator() && !fun->isAsync());

  // FIXME: This would be faster if we could avoid doing a lookup to get
  // the prototype for the instance.  Bug 906600.
  RootedValue pval(cx);
  if (!GetProperty(cx, fun, fun, cx->names().prototype, &pval)) {
    return nullptr;
  }
  RootedObject proto(cx, pval.isObject() ? &pval.toObject() : nullptr);
  if (!proto) {
    proto = GlobalObject::getOrCreateGeneratorObjectPrototype(cx, cx->global());
    if (!proto) {
      return nullptr;
    }
  }
  return NewObjectWithGivenProto<GeneratorObject>(cx, proto);
}

const JSClass GeneratorObject::class_ = {
    "Generator",
    JSCLASS_HAS_RESERVED_SLOTS(GeneratorObject::RESERVED_SLOTS),
    &classOps_,
};

const JSClassOps GeneratorObject::classOps_ = {
    nullptr,                                   // addProperty
    nullptr,                                   // delProperty
    nullptr,                                   // enumerate
    nullptr,                                   // newEnumerate
    nullptr,                                   // resolve
    nullptr,                                   // mayResolve
    nullptr,                                   // finalize
    nullptr,                                   // call
    nullptr,                                   // hasInstance
    nullptr,                                   // construct
    CallTraceMethod<AbstractGeneratorObject>,  // trace
};

static const JSFunctionSpec generator_methods[] = {
    JS_SELF_HOSTED_FN("next", "GeneratorNext", 1, 0),
    JS_SELF_HOSTED_FN("throw", "GeneratorThrow", 1, 0),
    JS_SELF_HOSTED_FN("return", "GeneratorReturn", 1, 0), JS_FS_END};

JSObject* js::NewTenuredObjectWithFunctionPrototype(
    JSContext* cx, Handle<GlobalObject*> global) {
  RootedObject proto(cx,
                     GlobalObject::getOrCreateFunctionPrototype(cx, global));
  if (!proto) {
    return nullptr;
  }
  return NewTenuredObjectWithGivenProto<PlainObject>(cx, proto);
}

static JSObject* CreateGeneratorFunction(JSContext* cx, JSProtoKey key) {
  RootedObject proto(
      cx, GlobalObject::getOrCreateFunctionConstructor(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }

  HandlePropertyName name = cx->names().GeneratorFunction;
  return NewFunctionWithProto(cx, Generator, 1, FunctionFlags::NATIVE_CTOR,
                              nullptr, name, proto, gc::AllocKind::FUNCTION,
                              TenuredObject);
}

static JSObject* CreateGeneratorFunctionPrototype(JSContext* cx,
                                                  JSProtoKey key) {
  return NewTenuredObjectWithFunctionPrototype(cx, cx->global());
}

static bool GeneratorFunctionClassFinish(JSContext* cx,
                                         HandleObject genFunction,
                                         HandleObject genFunctionProto) {
  Handle<GlobalObject*> global = cx->global();

  // Change the "constructor" property to non-writable before adding any other
  // properties, so it's still the last property and can be modified without a
  // dictionary-mode transition.
  MOZ_ASSERT(genFunctionProto->as<NativeObject>().getLastProperty().key() ==
             NameToId(cx->names().constructor));
  MOZ_ASSERT(!genFunctionProto->as<NativeObject>().inDictionaryMode());

  RootedValue genFunctionVal(cx, ObjectValue(*genFunction));
  if (!DefineDataProperty(cx, genFunctionProto, cx->names().constructor,
                          genFunctionVal, JSPROP_READONLY)) {
    return false;
  }
  MOZ_ASSERT(!genFunctionProto->as<NativeObject>().inDictionaryMode());

  RootedObject iteratorProto(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
  if (!iteratorProto) {
    return false;
  }

  RootedObject genObjectProto(cx, GlobalObject::createBlankPrototypeInheriting(
                                      cx, &PlainObject::class_, iteratorProto));
  if (!genObjectProto) {
    return false;
  }
  if (!DefinePropertiesAndFunctions(cx, genObjectProto, nullptr,
                                    generator_methods) ||
      !DefineToStringTag(cx, genObjectProto, cx->names().Generator)) {
    return false;
  }

  if (!LinkConstructorAndPrototype(cx, genFunctionProto, genObjectProto,
                                   JSPROP_READONLY, JSPROP_READONLY) ||
      !DefineToStringTag(cx, genFunctionProto, cx->names().GeneratorFunction)) {
    return false;
  }

  global->setGeneratorObjectPrototype(genObjectProto);

  return true;
}

static const ClassSpec GeneratorFunctionClassSpec = {
    CreateGeneratorFunction,
    CreateGeneratorFunctionPrototype,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    GeneratorFunctionClassFinish,
    ClassSpec::DontDefineConstructor};

const JSClass js::GeneratorFunctionClass = {
    "GeneratorFunction", 0, JS_NULL_CLASS_OPS, &GeneratorFunctionClassSpec};

const Value& AbstractGeneratorObject::getUnaliasedLocal(uint32_t slot) const {
  MOZ_ASSERT(isSuspended());
  MOZ_ASSERT(hasStackStorage());
  MOZ_ASSERT(slot < callee().nonLazyScript()->nfixed());
  return stackStorage().getDenseElement(slot);
}

void AbstractGeneratorObject::setUnaliasedLocal(uint32_t slot,
                                                const Value& value) {
  MOZ_ASSERT(isSuspended());
  MOZ_ASSERT(hasStackStorage());
  MOZ_ASSERT(slot < callee().nonLazyScript()->nfixed());
  return stackStorage().setDenseElement(slot, value);
}

bool AbstractGeneratorObject::isAfterYield() {
  return isAfterYieldOrAwait(JSOp::Yield);
}

bool AbstractGeneratorObject::isAfterAwait() {
  return isAfterYieldOrAwait(JSOp::Await);
}

bool AbstractGeneratorObject::isAfterYieldOrAwait(JSOp op) {
  if (isClosed() || isRunning()) {
    return false;
  }

  JSScript* script = callee().nonLazyScript();
  jsbytecode* code = script->code();
  uint32_t nextOffset = script->resumeOffsets()[resumeIndex()];
  if (JSOp(code[nextOffset]) != JSOp::AfterYield) {
    return false;
  }

  static_assert(JSOpLength_Yield == JSOpLength_InitialYield,
                "JSOp::Yield and JSOp::InitialYield must have the same length");
  static_assert(JSOpLength_Yield == JSOpLength_Await,
                "JSOp::Yield and JSOp::Await must have the same length");

  uint32_t offset = nextOffset - JSOpLength_Yield;
  JSOp prevOp = JSOp(code[offset]);
  MOZ_ASSERT(prevOp == JSOp::InitialYield || prevOp == JSOp::Yield ||
             prevOp == JSOp::Await);

  return prevOp == op;
}

template <>
bool JSObject::is<js::AbstractGeneratorObject>() const {
  return is<GeneratorObject>() || is<AsyncFunctionGeneratorObject>() ||
         is<AsyncGeneratorObject>();
}

GeneratorResumeKind js::ParserAtomToResumeKind(
    JSContext* cx, frontend::TaggedParserAtomIndex atom) {
  if (atom == frontend::TaggedParserAtomIndex::WellKnown::next()) {
    return GeneratorResumeKind::Next;
  }
  if (atom == frontend::TaggedParserAtomIndex::WellKnown::throw_()) {
    return GeneratorResumeKind::Throw;
  }
  MOZ_ASSERT(atom == frontend::TaggedParserAtomIndex::WellKnown::return_());
  return GeneratorResumeKind::Return;
}

JSAtom* js::ResumeKindToAtom(JSContext* cx, GeneratorResumeKind kind) {
  switch (kind) {
    case GeneratorResumeKind::Next:
      return cx->names().next;

    case GeneratorResumeKind::Throw:
      return cx->names().throw_;

    case GeneratorResumeKind::Return:
      return cx->names().return_;
  }
  MOZ_CRASH("Invalid resume kind");
}
