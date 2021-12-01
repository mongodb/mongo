/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
 * per-global and not leak, we create a js::Class to wrap the C++ instance and
 * provide an appropriate finalizer. We lazily create and store an instance of
 * that js::Class in a global reserved slot.
 */

static void
resc_finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());
    RegExpStatics* res = static_cast<RegExpStatics*>(obj->as<RegExpStaticsObject>().getPrivate());
    fop->delete_(res);
}

static void
resc_trace(JSTracer* trc, JSObject* obj)
{
    void* pdata = obj->as<RegExpStaticsObject>().getPrivate();
    if (pdata)
        static_cast<RegExpStatics*>(pdata)->trace(trc);
}

static const ClassOps RegExpStaticsObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    resc_finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    resc_trace
};

const Class RegExpStaticsObject::class_ = {
    "RegExpStatics",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_FOREGROUND_FINALIZE,
    &RegExpStaticsObjectClassOps
};

RegExpStaticsObject*
RegExpStatics::create(JSContext* cx)
{
    RegExpStaticsObject* obj = NewObjectWithGivenProto<RegExpStaticsObject>(cx, nullptr);
    if (!obj)
        return nullptr;
    RegExpStatics* res = cx->new_<RegExpStatics>();
    if (!res)
        return nullptr;
    obj->setPrivate(static_cast<void*>(res));
    return obj;
}

bool
RegExpStatics::executeLazy(JSContext* cx)
{
    if (!pendingLazyEvaluation)
        return true;

    MOZ_ASSERT(lazySource);
    MOZ_ASSERT(matchesInput);
    MOZ_ASSERT(lazyIndex != size_t(-1));

    /* Retrieve or create the RegExpShared in this zone. */
    RootedAtom source(cx, lazySource);
    RootedRegExpShared shared(cx, cx->zone()->regExps.get(cx, source, lazyFlags));
    if (!shared)
        return false;

    /*
     * It is not necessary to call aboutToWrite(): evaluation of
     * implicit copies is safe.
     */

    /* Execute the full regular expression. */
    RootedLinearString input(cx, matchesInput);
    RegExpRunStatus status = RegExpShared::execute(cx, &shared, input, lazyIndex, &this->matches,
                                                   nullptr);
    if (status == RegExpRunStatus_Error)
        return false;

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
