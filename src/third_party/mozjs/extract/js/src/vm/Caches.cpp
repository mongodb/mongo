/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Caches-inl.h"

#include "mozilla/PodOperations.h"

using namespace js;

using mozilla::PodZero;

void NewObjectCache::clearNurseryObjects(JSRuntime* rt) {
  for (auto& e : entries) {
    NativeObject* obj = reinterpret_cast<NativeObject*>(&e.templateObject);
    if (IsInsideNursery(e.key) || rt->gc.nursery().isInside(obj->slots_) ||
        rt->gc.nursery().isInside(obj->elements_)) {
      PodZero(&e);
    }
  }
}
