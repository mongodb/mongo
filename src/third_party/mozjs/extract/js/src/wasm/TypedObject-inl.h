/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_TypedObject_inl_h
#define wasm_TypedObject_inl_h

#include "wasm/TypedObject.h"

#include "gc/ObjectKind-inl.h"

/* static */
js::gc::AllocKind js::InlineTypedObject::allocKindForRttValue(RttValue* rtt) {
  size_t nbytes = rtt->size();
  MOZ_ASSERT(nbytes <= MaxInlineBytes);
  MOZ_ASSERT(rtt->kind() == wasm::TypeDefKind::Struct);

  return gc::GetGCObjectKindForBytes(nbytes + sizeof(TypedObject));
}

#endif  // wasm_TypedObject_inl_h
