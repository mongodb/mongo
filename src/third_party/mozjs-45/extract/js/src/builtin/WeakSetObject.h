/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakSetObject_h
#define builtin_WeakSetObject_h

#include "vm/NativeObject.h"

namespace js {

class WeakSetObject : public NativeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;

    static JSObject* initClass(JSContext* cx, JSObject* obj);
    static const Class class_;

  private:
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];

    static WeakSetObject* create(JSContext* cx, HandleObject proto = nullptr);
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
};

extern JSObject*
InitWeakSetClass(JSContext* cx, HandleObject obj);

} // namespace js

#endif /* builtin_WeakSetObject_h */
