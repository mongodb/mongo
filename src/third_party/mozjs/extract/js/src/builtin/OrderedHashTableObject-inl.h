/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_OrderedHashTableObject_inl_h
#define builtin_OrderedHashTableObject_inl_h

#include "builtin/OrderedHashTableObject.h"

#include "gc/Nursery-inl.h"

inline void* js::detail::OrderedHashTableObject::allocateCellBuffer(
    JSContext* cx, size_t numBytes) {
  return AllocNurseryOrMallocBuffer<uint8_t>(cx, this, numBytes);
}

#endif /* builtin_OrderedHashTableObject_inl_h */
