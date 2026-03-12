/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DisposableRecord.h"

#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using namespace js;

/* static */ SharedShape* DisposableRecordObject::assignInitialShape(
    JSContext* cx, Handle<DisposableRecordObject*> self) {
  MOZ_ASSERT(self->empty());

  constexpr PropertyFlags flags = {PropertyFlag::Writable};

  if (!NativeObject::addPropertyInReservedSlot(
          cx, self, cx->names().value, DisposableRecordObject::VALUE_SLOT,
          flags)) {
    return nullptr;
  }

  if (!NativeObject::addPropertyInReservedSlot(
          cx, self, cx->names().method, DisposableRecordObject::METHOD_SLOT,
          flags)) {
    return nullptr;
  }

  if (!NativeObject::addPropertyInReservedSlot(
          cx, self, cx->names().hint, DisposableRecordObject::HINT_SLOT,
          flags)) {
    return nullptr;
  }

  return self->sharedShape();
}

const JSClass DisposableRecordObject::class_ = {
    "DisposableRecord",
    JSCLASS_HAS_RESERVED_SLOTS(DisposableRecordObject::RESERVED_SLOTS),
};
