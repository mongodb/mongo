/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_WeakMapObject_h
#define builtin_WeakMapObject_h

#include "gc/WeakMap.h"
#include "vm/JSObject.h"

namespace js {

// Abstract base class for WeakMapObject and WeakSetObject.
class WeakCollectionObject : public NativeObject
{
  public:
    ObjectValueMap* getMap() { return static_cast<ObjectValueMap*>(getPrivate()); }

    static MOZ_MUST_USE bool nondeterministicGetKeys(JSContext* cx,
                                                     Handle<WeakCollectionObject*> obj,
                                                     MutableHandleObject ret);

  protected:
    static const ClassOps classOps_;
};

class WeakMapObject : public WeakCollectionObject
{
  public:
    static const Class class_;
};

extern JSObject*
InitWeakMapClass(JSContext* cx, HandleObject obj);

} // namespace js

#endif /* builtin_WeakMapObject_h */
