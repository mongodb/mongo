/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DisposableRecord_h
#define vm_DisposableRecord_h

#include "NamespaceImports.h"
#include "js/Value.h"
#include "vm/NativeObject.h"
#include "vm/UsingHint.h"

namespace js {

/**
 * Explicit Resource Management Proposal
 * DisposableResource Records
 * https://arai-a.github.io/ecma262-compare/?pr=3000&id=sec-disposableresource-records
 */

class DisposableRecordObject : public NativeObject {
 public:
  static const JSClass class_;

  static constexpr uint32_t VALUE_SLOT = 0;
  static constexpr uint32_t METHOD_SLOT = 1;
  static constexpr uint32_t HINT_SLOT = 2;

  static constexpr uint32_t RESERVED_SLOTS = 3;

  [[nodiscard]] inline static DisposableRecordObject* create(
      JSContext* cx, JS::Handle<JS::Value> value, JS::Handle<JS::Value> method,
      UsingHint hint);

  Value getObject() { return getReservedSlot(VALUE_SLOT); }

  Value getMethod() { return getReservedSlot(METHOD_SLOT); }

  UsingHint getHint() {
    Value hint = getReservedSlot(HINT_SLOT);
    UsingHint hintVal = UsingHint(hint.toInt32());
    return hintVal;
  }

  static SharedShape* assignInitialShape(JSContext* cx,
                                         Handle<DisposableRecordObject*> self);
};

}  // namespace js

#endif  // vm_DisposableRecord_h
