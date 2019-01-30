/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakMapObject_inl_h
#define builtin_WeakMapObject_inl_h

#include "builtin/WeakMapObject.h"

#include "vm/ProxyObject.h"

namespace js {

static bool
TryPreserveReflector(JSContext* cx, HandleObject obj)
{
    if (obj->getClass()->isWrappedNative() ||
        obj->getClass()->isDOMClass() ||
        (obj->is<ProxyObject>() &&
         obj->as<ProxyObject>().handler()->family() == GetDOMProxyHandlerFamily()))
    {
        MOZ_ASSERT(cx->runtime()->preserveWrapperCallback);
        if (!cx->runtime()->preserveWrapperCallback(cx, obj)) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_WEAKMAP_KEY);
            return false;
        }
    }
    return true;
}

static MOZ_ALWAYS_INLINE bool
WeakCollectionPutEntryInternal(JSContext* cx, Handle<WeakCollectionObject*> obj,
                               HandleObject key, HandleValue value)
{
    ObjectValueMap* map = obj->getMap();
    if (!map) {
        auto newMap = cx->make_unique<ObjectValueMap>(cx, obj.get());
        if (!newMap)
            return false;
        if (!newMap->init()) {
            JS_ReportOutOfMemory(cx);
            return false;
        }
        map = newMap.release();
        obj->setPrivate(map);
    }

    // Preserve wrapped native keys to prevent wrapper optimization.
    if (!TryPreserveReflector(cx, key))
        return false;

    if (JSWeakmapKeyDelegateOp op = key->getClass()->extWeakmapKeyDelegateOp()) {
        RootedObject delegate(cx, op(key));
        if (delegate && !TryPreserveReflector(cx, delegate))
            return false;
    }

    MOZ_ASSERT(key->compartment() == obj->compartment());
    MOZ_ASSERT_IF(value.isObject(), value.toObject().compartment() == obj->compartment());
    if (!map->put(key, value)) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

} // namespace js

#endif /* builtin_WeakMapObject_inl_h */
