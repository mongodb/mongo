/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Caches-inl.h"

#include "mozilla/PodOperations.h"

using namespace js;

using mozilla::PodZero;

MathCache*
RuntimeCaches::createMathCache(JSContext* cx)
{
    MOZ_ASSERT(!mathCache_);

    UniquePtr<MathCache> newMathCache(js_new<MathCache>());
    if (!newMathCache) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    mathCache_ = Move(newMathCache);
    return mathCache_.get();
}

bool
RuntimeCaches::init()
{
    if (!evalCache.init())
        return false;

    return true;
}

void
NewObjectCache::clearNurseryObjects(JSRuntime* rt)
{
    for (unsigned i = 0; i < mozilla::ArrayLength(entries); ++i) {
        Entry& e = entries[i];
        NativeObject* obj = reinterpret_cast<NativeObject*>(&e.templateObject);
        if (IsInsideNursery(e.key) ||
            rt->gc.nursery().isInside(obj->slots_) ||
            rt->gc.nursery().isInside(obj->elements_))
        {
            PodZero(&e);
        }
    }
}
