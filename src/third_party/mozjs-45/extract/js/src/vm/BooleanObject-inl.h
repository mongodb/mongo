/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_BooleanObject_inl_h
#define vm_BooleanObject_inl_h

#include "vm/BooleanObject.h"

#include "jsobjinlines.h"

namespace js {

inline BooleanObject*
BooleanObject::create(JSContext* cx, bool b, HandleObject proto /* = nullptr */)
{
    BooleanObject* obj = NewObjectWithClassProto<BooleanObject>(cx, proto);
    if (!obj)
        return nullptr;
    obj->setPrimitiveValue(b);
    return obj;
}

} // namespace js

#endif /* vm_BooleanObject_inl_h */
