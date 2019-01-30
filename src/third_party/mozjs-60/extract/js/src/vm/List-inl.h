/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_List_inl_h
#define vm_List_inl_h

#include "gc/Rooting.h"
#include "vm/JSContext.h"
#include "vm/NativeObject.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

namespace js {

inline MOZ_MUST_USE NativeObject*
NewList(JSContext* cx)
{
    return NewObjectWithNullTaggedProto<PlainObject>(cx);
}

inline MOZ_MUST_USE bool
AppendToList(JSContext* cx, HandleNativeObject list, HandleValue value)
{
    uint32_t length = list->getDenseInitializedLength();

    if (!list->ensureElements(cx, length + 1))
        return false;

    list->ensureDenseInitializedLength(cx, length, 1);
    list->setDenseElementWithType(cx, length, value);

    return true;
}

template<class T>
inline MOZ_MUST_USE T*
PeekList(NativeObject* list)
{
    MOZ_ASSERT(list->getDenseInitializedLength() > 0);
    return &list->getDenseElement(0).toObject().as<T>();
}

template<class T>
inline MOZ_MUST_USE T*
ShiftFromList(JSContext* cx, HandleNativeObject list)
{
    uint32_t length = list->getDenseInitializedLength();
    MOZ_ASSERT(length > 0);

    Rooted<T*> entry(cx, &list->getDenseElement(0).toObject().as<T>());
    if (!list->tryShiftDenseElements(1)) {
        list->moveDenseElements(0, 1, length - 1);
        list->setDenseInitializedLength(length - 1);
        list->shrinkElements(cx, length - 1);
    }

    MOZ_ASSERT(list->getDenseInitializedLength() == length - 1);
    return entry;
}

} /* namespace js */

#endif /* vm_List_inl_h */
