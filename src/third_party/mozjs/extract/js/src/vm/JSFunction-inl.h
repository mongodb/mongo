/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSFunction_inl_h
#define vm_JSFunction_inl_h

#include "vm/JSFunction.h"

#include "gc/GCProbes.h"

#include "gc/ObjectKind-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

inline const char* GetFunctionNameBytes(JSContext* cx, JSFunction* fun,
                                        UniqueChars* bytes) {
  if (fun->isAccessorWithLazyName()) {
    JSAtom* name = fun->getAccessorNameForLazy(cx);
    if (!name) {
      return nullptr;
    }

    *bytes = StringToNewUTF8CharsZ(cx, *name);
    return bytes->get();
  }

  if (JSAtom* name = fun->fullExplicitName()) {
    *bytes = StringToNewUTF8CharsZ(cx, *name);
    return bytes->get();
  }

  return "anonymous";
}

} /* namespace js */

/* static */
inline JSFunction* JSFunction::create(JSContext* cx, js::gc::AllocKind kind,
                                      js::gc::Heap heap,
                                      js::Handle<js::SharedShape*> shape) {
  MOZ_ASSERT(kind == js::gc::AllocKind::FUNCTION ||
             kind == js::gc::AllocKind::FUNCTION_EXTENDED);

  debugCheckNewObject(shape, kind, heap);

  const JSClass* clasp = shape->getObjectClass();
  MOZ_ASSERT(clasp->isNativeObject());
  MOZ_ASSERT(clasp->isJSFunction());
  MOZ_ASSERT_IF(kind == js::gc::AllocKind::FUNCTION,
                clasp == js::FunctionClassPtr);
  MOZ_ASSERT_IF(kind == js::gc::AllocKind::FUNCTION_EXTENDED,
                clasp == js::FunctionExtendedClassPtr);

  MOZ_ASSERT(calculateDynamicSlots(shape->numFixedSlots(), shape->slotSpan(),
                                   clasp) == 0);

  NativeObject* nobj = cx->newCell<NativeObject>(kind, heap, clasp);
  if (!nobj) {
    return nullptr;
  }

  nobj->initShape(shape);

  nobj->initEmptyDynamicSlots();
  nobj->setEmptyElements();

  JSFunction* fun = static_cast<JSFunction*>(nobj);
  fun->initFixedSlots(JSCLASS_RESERVED_SLOTS(clasp));
  fun->initFlagsAndArgCount();
  fun->initFixedSlot(NativeJitInfoOrInterpretedScriptSlot,
                     JS::PrivateValue(nullptr));

  if (kind == js::gc::AllocKind::FUNCTION_EXTENDED) {
    fun->setFlags(FunctionFlags::EXTENDED);
  }

  MOZ_ASSERT(!clasp->shouldDelayMetadataBuilder(),
             "Function has no extra data hanging off it, that wouldn't be "
             "allocated at this point, that would require delaying the "
             "building of metadata for it");
  if (MOZ_UNLIKELY(cx->realm()->hasAllocationMetadataBuilder())) {
    fun = SetNewObjectMetadata(cx, fun);
  }

  js::gc::gcprobes::CreateObject(fun);

  return fun;
}

/* static */
inline bool JSFunction::getLength(JSContext* cx, js::HandleFunction fun,
                                  uint16_t* length) {
  if (fun->isNativeFun()) {
    *length = fun->nargs();
    return true;
  }

  JSScript* script = getOrCreateScript(cx, fun);
  if (!script) {
    return false;
  }

  *length = script->funLength();
  return true;
}

/* static */
inline bool JSFunction::getUnresolvedLength(JSContext* cx,
                                            js::HandleFunction fun,
                                            uint16_t* length) {
  MOZ_ASSERT(!IsInternalFunctionObject(*fun));
  MOZ_ASSERT(!fun->hasResolvedLength());

  return JSFunction::getLength(cx, fun, length);
}

inline JSAtom* JSFunction::getUnresolvedName(JSContext* cx) {
  if (isAccessorWithLazyName()) {
    return getAccessorNameForLazy(cx);
  }

  return infallibleGetUnresolvedName(cx);
}

inline JSAtom* JSFunction::infallibleGetUnresolvedName(JSContext* cx) {
  MOZ_ASSERT(!IsInternalFunctionObject(*this));
  MOZ_ASSERT(!isAccessorWithLazyName());
  MOZ_ASSERT(!hasResolvedName());

  if (JSAtom* name = fullExplicitOrInferredName()) {
    return name;
  }

  return cx->names().empty_;
}

/* static */ inline bool JSFunction::getAllocKindForThis(
    JSContext* cx, js::HandleFunction func, js::gc::AllocKind& allocKind) {
  JSScript* script = getOrCreateScript(cx, func);
  if (!script) {
    return false;
  }

  size_t propertyCountEstimate =
      script->immutableScriptData()->propertyCountEstimate;

  // Choose the alloc assuming at least the default NewObjectKind slots, but
  // bigger if our estimate shows we need it.
  allocKind = js::gc::GetGCObjectKind(std::max(
      js::gc::GetGCKindSlots(js::NewObjectGCKind()), propertyCountEstimate));
  return true;
}

#endif /* vm_JSFunction_inl_h */
