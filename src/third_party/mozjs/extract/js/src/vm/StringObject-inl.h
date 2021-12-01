/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StringObject_inl_h
#define vm_StringObject_inl_h

#include "vm/StringObject.h"

#include "vm/JSObject-inl.h"
#include "vm/Shape-inl.h"

namespace js {

/* static */ inline bool
StringObject::init(JSContext* cx, Handle<StringObject*> obj, HandleString str)
{
    MOZ_ASSERT(obj->numFixedSlots() == 2);

    if (!EmptyShape::ensureInitialCustomShape<StringObject>(cx, obj))
        return false;

    MOZ_ASSERT(obj->lookup(cx, NameToId(cx->names().length))->slot() == LENGTH_SLOT);

    obj->setStringThis(str);

    return true;
}

/* static */ inline StringObject*
StringObject::create(JSContext* cx, HandleString str, HandleObject proto, NewObjectKind newKind)
{
    JSObject* obj = NewObjectWithClassProto(cx, &class_, proto, newKind);
    if (!obj)
        return nullptr;
    Rooted<StringObject*> strobj(cx, &obj->as<StringObject>());
    if (!StringObject::init(cx, strobj, str))
        return nullptr;
    return strobj;
}

} // namespace js

#endif /* vm_StringObject_inl_h */
