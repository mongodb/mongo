/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_RawJSONObject_h
#define builtin_RawJSONObject_h

#include "vm/NativeObject.h"

namespace js {

class RawJSONObject : public NativeObject {
  enum { SlotCount = 0 };

 public:
  static const JSClass class_;

  static RawJSONObject* create(JSContext* cx, Handle<JSString*> jsonString);

  JSString* rawJSON(JSContext* cx);
};

}  // namespace js

#endif /* builtin_RawJSONObject_h */
