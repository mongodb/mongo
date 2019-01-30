/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ErrorObject_inl_h
#define vm_ErrorObject_inl_h

#include "vm/ErrorObject.h"

#include "vm/JSContext.h"

inline JSString*
js::ErrorObject::fileName(JSContext* cx) const
{
    const HeapSlot& slot = getReservedSlotRef(FILENAME_SLOT);
    return slot.isString() ? slot.toString() : cx->names().empty;
}

inline uint32_t
js::ErrorObject::lineNumber() const
{
    const HeapSlot& slot = getReservedSlotRef(LINENUMBER_SLOT);
    return slot.isInt32() ? slot.toInt32() : 0;
}

inline uint32_t
js::ErrorObject::columnNumber() const
{
    const HeapSlot& slot = getReservedSlotRef(COLUMNNUMBER_SLOT);
    return slot.isInt32() ? slot.toInt32() : 0;
}

inline JSObject*
js::ErrorObject::stack() const
{
    return getReservedSlotRef(STACK_SLOT).toObjectOrNull();
}

#endif /* vm_ErrorObject_inl_h */
