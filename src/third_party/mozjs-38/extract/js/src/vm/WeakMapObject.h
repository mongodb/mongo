/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_WeakMapObject_h
#define vm_WeakMapObject_h

#include "jsobj.h"
#include "jsweakmap.h"

namespace js {

class ObjectValueMap : public WeakMap<PreBarrieredObject, RelocatableValue>
{
  public:
    ObjectValueMap(JSContext* cx, JSObject* obj)
      : WeakMap<PreBarrieredObject, RelocatableValue>(cx, obj) {}

    virtual bool findZoneEdges();
};

class WeakMapObject : public NativeObject
{
  public:
    static const Class class_;

    ObjectValueMap* getMap() { return static_cast<ObjectValueMap*>(getPrivate()); }
};

} // namespace js

#endif /* vm_WeakMapObject_h */
