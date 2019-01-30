/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ShapedObject_inl_h
#define vm_ShapedObject_inl_h

#include "vm/ShapedObject.h"

#include "jsfriendapi.h"

#include "js/Proxy.h"

template<>
inline bool
JSObject::is<js::ShapedObject>() const
{
    return isNative() || is<js::ProxyObject>() || is<js::TypedObject>();
}

#endif /* vm_ShapedObject_inl_h */
