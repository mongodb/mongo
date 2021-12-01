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
#include "vm/SharedMem.h"

namespace js {

inline SharedMem<uint8_t*>
ArrayBufferObjectMaybeShared::dataPointerEither()
{
    ArrayBufferObjectMaybeShared* buf = this;
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().dataPointerShared();
    return buf->as<SharedArrayBufferObject>().dataPointerShared();
}

inline bool
ArrayBufferObjectMaybeShared::isDetached() const
{
    if (this->is<ArrayBufferObject>())
        return this->as<ArrayBufferObject>().isDetached();
    return false;
}

inline uint32_t
AnyArrayBufferByteLength(const ArrayBufferObjectMaybeShared* buf)
{
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().byteLength();
    return buf->as<SharedArrayBufferObject>().byteLength();
}

inline uint32_t
ArrayBufferObjectMaybeShared::byteLength()
{
    return AnyArrayBufferByteLength(this);
}

inline bool
AnyArrayBufferIsPreparedForAsmJS(const ArrayBufferObjectMaybeShared* buf)
{
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().isPreparedForAsmJS();
    return buf->as<SharedArrayBufferObject>().isPreparedForAsmJS();
}

inline bool
ArrayBufferObjectMaybeShared::isPreparedForAsmJS() const
{
    return AnyArrayBufferIsPreparedForAsmJS(this);
}

inline bool
AnyArrayBufferIsWasm(const ArrayBufferObjectMaybeShared* buf)
{
    if (buf->is<ArrayBufferObject>())
        return buf->as<ArrayBufferObject>().isWasm();
    return buf->as<SharedArrayBufferObject>().isWasm();
}

inline bool
ArrayBufferObjectMaybeShared::isWasm() const
{
    return AnyArrayBufferIsWasm(this);
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
