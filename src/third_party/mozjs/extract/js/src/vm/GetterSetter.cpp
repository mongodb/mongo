/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/GetterSetter.h"

#include "gc/Allocator.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"

using namespace js;

js::GetterSetter::GetterSetter(HandleObject getter, HandleObject setter)
    : TenuredCellWithGCPointer(getter), setter_(setter) {}

// static
GetterSetter* GetterSetter::create(JSContext* cx, HandleObject getter,
                                   HandleObject setter) {
  return cx->newCell<GetterSetter>(getter, setter);
}

JS::ubi::Node::Size JS::ubi::Concrete<GetterSetter>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return js::gc::Arena::thingSize(get().asTenured().getAllocKind());
}
