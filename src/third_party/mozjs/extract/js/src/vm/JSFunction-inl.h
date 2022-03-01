/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSFunction_inl_h
#define vm_JSFunction_inl_h

#include "vm/JSFunction.h"

#include "gc/Allocator.h"
#include "gc/GCProbes.h"
#include "js/CharacterEncoding.h"
#include "vm/EnvironmentObject.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

inline const char* GetFunctionNameBytes(JSContext* cx, JSFunction* fun,
                                        UniqueChars* bytes) {
  if (JSAtom* name = fun->explicitName()) {
    *bytes = StringToNewUTF8CharsZ(cx, *name);
    return bytes->get();
  }
  return js_anonymous_str;
}

inline JSFunction* CloneFunctionObject(JSContext* cx, HandleFunction fun,
                                       HandleObject enclosingEnv,
                                       HandleObject proto = nullptr) {
  // These intermediate variables are needed to avoid link errors on some
  // platforms.  Sigh.
  gc::AllocKind finalizeKind = gc::AllocKind::FUNCTION;
  gc::AllocKind extendedFinalizeKind = gc::AllocKind::FUNCTION_EXTENDED;
  gc::AllocKind kind = fun->isExtended() ? extendedFinalizeKind : finalizeKind;

  MOZ_ASSERT(CanReuseScriptForClone(cx->realm(), fun, enclosingEnv));
  return CloneFunctionReuseScript(cx, fun, enclosingEnv, kind, proto);
}

} /* namespace js */

/* static */ inline JS::Result<JSFunction*, JS::OOM> JSFunction::create(
    JSContext* cx, js::gc::AllocKind kind, js::gc::InitialHeap heap,
    js::HandleShape shape) {
  MOZ_ASSERT(kind == js::gc::AllocKind::FUNCTION ||
             kind == js::gc::AllocKind::FUNCTION_EXTENDED);

  debugCheckNewObject(shape, kind, heap);

  const JSClass* clasp = shape->getObjectClass();
  MOZ_ASSERT(clasp->isNativeObject());
  MOZ_ASSERT(clasp->isJSFunction());

  static constexpr size_t NumDynamicSlots = 0;
  MOZ_ASSERT(calculateDynamicSlots(shape->numFixedSlots(), shape->slotSpan(),
                                   clasp) == NumDynamicSlots);

  JSObject* obj = js::AllocateObject(cx, kind, NumDynamicSlots, heap, clasp);
  if (!obj) {
    return cx->alreadyReportedOOM();
  }

  NativeObject* nobj = static_cast<NativeObject*>(obj);
  nobj->initShape(shape);

  nobj->initEmptyDynamicSlots();
  nobj->setEmptyElements();

  MOZ_ASSERT(!clasp->hasPrivate());
  MOZ_ASSERT(shape->slotSpan() == 0);

  JSFunction* fun = static_cast<JSFunction*>(nobj);
  fun->nargs_ = 0;

  // This must be overwritten by some ultimate caller: there's no default
  // value to which we could sensibly initialize this.
  MOZ_MAKE_MEM_UNDEFINED(&fun->u, sizeof(u));

  fun->atom_.init(nullptr);

  if (kind == js::gc::AllocKind::FUNCTION_EXTENDED) {
    fun->setFlags(FunctionFlags::EXTENDED);
    for (js::GCPtrValue& extendedSlot : fun->toExtended()->extendedSlots) {
      extendedSlot.init(JS::UndefinedValue());
    }
  } else {
    fun->setFlags(0);
  }

  MOZ_ASSERT(!clasp->shouldDelayMetadataBuilder(),
             "Function has no extra data hanging off it, that wouldn't be "
             "allocated at this point, that would require delaying the "
             "building of metadata for it");
  fun = SetNewObjectMetadata(cx, fun);

  js::gc::gcprobes::CreateObject(fun);

  return fun;
}

#endif /* vm_JSFunction_inl_h */
