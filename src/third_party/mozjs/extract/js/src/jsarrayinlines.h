/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_jsarrayinlines_h
#define vm_jsarrayinlines_h

#include "jsarray.h"

#include "vm/ArgumentsObject.h"
#include "vm/JSObject.h"

#include "vm/ArgumentsObject-inl.h"
#include "vm/UnboxedObject-inl.h"

namespace js {

inline bool
GetElement(JSContext* cx, HandleObject obj, uint32_t index, MutableHandleValue vp)
{
    if (obj->isNative() && index < obj->as<NativeObject>().getDenseInitializedLength()) {
        vp.set(obj->as<NativeObject>().getDenseElement(index));
        if (!vp.isMagic(JS_ELEMENTS_HOLE))
            return true;
    }

    if (obj->is<ArgumentsObject>()) {
        if (obj->as<ArgumentsObject>().maybeGetElement(index, vp))
            return true;
    }

    return GetElement(cx, obj, obj, index, vp);
}

} // namespace js

#endif // vm_jsarrayinlines_h
