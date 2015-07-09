/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferObject_inl_h
#define vm_ArrayBufferObject_inl_h

/* Utilities and common inline code for ArrayBufferObject and SharedArrayBufferObject */

#include "vm/ArrayBufferObject.h"

#include "js/Value.h"

#include "vm/SharedArrayObject.h"

namespace js {

inline uint32_t
AnyArrayBufferByteLength(const ArrayBufferObjectMaybeShared* buf)
{
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().byteLength();
    return buf->as<SharedArrayBufferObject>().byteLength();
}

inline uint8_t*
AnyArrayBufferDataPointer(const ArrayBufferObjectMaybeShared* buf)
{
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().dataPointer();
    return buf->as<SharedArrayBufferObject>().dataPointer();
}

inline ArrayBufferObjectMaybeShared&
AsAnyArrayBuffer(HandleValue val)
{
    if (val.toObject().is<ArrayBufferObject>())
        return val.toObject().as<ArrayBufferObject>();
    return val.toObject().as<SharedArrayBufferObject>();
}

} // namespace js

#endif // vm_ArrayBufferObject_inl_h
