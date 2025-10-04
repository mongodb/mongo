/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_ShadowRealm_h
#define builtin_ShadowRealm_h

#include "vm/NativeObject.h"

namespace js {

class ShadowRealmObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass protoClass_;

  enum { GlobalSlot, SlotCount };

  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  Realm* getShadowRealm() {
    MOZ_ASSERT(getWrappedGlobal());
    return getWrappedGlobal()->nonCCWRealm();
  }

  JSObject* getWrappedGlobal() const {
    return &getFixedSlot(GlobalSlot).toObject();
  }
};

void ReportPotentiallyDetailedMessage(JSContext* cx,
                                      const unsigned detailedError,
                                      const unsigned genericError);
}  // namespace js

#endif
