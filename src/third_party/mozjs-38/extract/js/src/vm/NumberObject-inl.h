/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_NumberObject_inl_h
#define vm_NumberObject_inl_h

#include "vm/NumberObject.h"

#include "jsobjinlines.h"

namespace js {

inline NumberObject*
NumberObject::create(JSContext* cx, double d)
{
    JSObject* obj = NewBuiltinClassInstance(cx, &class_);
    if (!obj)
        return nullptr;
    NumberObject& numobj = obj->as<NumberObject>();
    numobj.setPrimitiveValue(d);
    return &numobj;
}

} // namespace js

#endif /* vm_NumberObject_inl_h */
