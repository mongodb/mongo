/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RegExpStatics.h"

#include "gc/FreeOp.h"
#include "vm/RegExpStaticsObject.h"

#include "vm/NativeObject-inl.h"

using namespace js;

/*
 * RegExpStatics allocates memory -- in order to keep the statics stored
 * per-global and not leak, we create a JSClass to wrap the C++ instance and
 * provide an appropriate finalizer. We lazily create and store an instance of
 * that JSClass in a global reserved slot.
 */

static void resc_finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());
  RegExpStatics* res =
      static_cast<RegExpStatics*>(obj->as<RegExpStaticsObject>().getPrivate());
  fop->delete_(obj, res, MemoryUse::RegExpStatics);
}

static void resc_trace(JSTracer* trc, JSObject* obj) {
  void* pdata = obj->as<RegExpStaticsObject>().getPrivate();
  if (pdata) {
    static_cast<RegExpStatics*>(pdata)->trace(trc);
  }
}

static const JSClassOps RegExpStaticsObjectClassOps = {
    nullptr,        // addProperty
    nullptr,        // delProperty
    nullptr,        // enumerate
    nullptr,        // newEnumerate
    nullptr,        // resolve
    nullptr,        // mayResolve
    resc_finalize,  // finalize
    nullptr,        // call
    nullptr,        // hasInstance
    nullptr,        // construct
    resc_trace,     // trace
};

const JSClass RegExpStaticsObject::class_ = {
    "RegExpStatics", JSCLASS_HAS_PRIVATE | JSCLASS_FOREGROUND_FINALIZE,
    &RegExpStaticsObjectClassOps};

RegExpStaticsObject* RegExpStatics::create(JSContext* cx) {
  RegExpStaticsObject* obj =
      NewObjectWithGivenProto<RegExpStaticsObject>(cx, nullptr);
  if (!obj) {
    return nullptr;
  }
  RegExpStatics* res = cx->new_<RegExpStatics>();
  if (!res) {
    return nullptr;
  }
  // TODO: This doesn't account for match vector heap memory used if there are
  // more 10 matches. This is likely to be rare.
  InitObjectPrivate(obj, res, MemoryUse::RegExpStatics);
  return obj;
}

bool RegExpStatics::executeLazy(JSContext* cx) {
  if (!pendingLazyEvaluation) {
    return true;
  }

  MOZ_ASSERT(lazySource);
  MOZ_ASSERT(matchesInput);
  MOZ_ASSERT(lazyIndex != size_t(-1));

  /* Retrieve or create the RegExpShared in this zone. */
  RootedAtom source(cx, lazySource);
  RootedRegExpShared shared(cx,
                            cx->zone()->regExps().get(cx, source, lazyFlags));
  if (!shared) {
    return false;
  }

  /*
   * It is not necessary to call aboutToWrite(): evaluation of
   * implicit copies is safe.
   */

  /* Execute the full regular expression. */
  RootedLinearString input(cx, matchesInput);
  RegExpRunStatus status =
      RegExpShared::execute(cx, &shared, input, lazyIndex, &this->matches);
  if (status == RegExpRunStatus_Error) {
    return false;
  }

  /*
   * RegExpStatics are only updated on successful (matching) execution.
   * Re-running the same expression must therefore produce a matching result.
   */
  MOZ_ASSERT(status == RegExpRunStatus_Success);

  /* Unset lazy state and remove rooted values that now have no use. */
  pendingLazyEvaluation = false;
  lazySource = nullptr;
  lazyIndex = size_t(-1);

  return true;
}
