/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Rooting_h
#define gc_Rooting_h

#include "js/RootingAPI.h"

class JSAtom;
class JSLinearString;

namespace js {

class PropertyName;
class NativeObject;
class ArrayObject;
class GlobalObject;
class PlainObject;
class ScriptSourceObject;
class Shape;
class ObjectGroup;

// These are internal counterparts to the public types such as HandleObject.

typedef JS::Handle<NativeObject*>      HandleNativeObject;
typedef JS::Handle<Shape*>             HandleShape;
typedef JS::Handle<ObjectGroup*>       HandleObjectGroup;
typedef JS::Handle<JSAtom*>            HandleAtom;
typedef JS::Handle<JSLinearString*>    HandleLinearString;
typedef JS::Handle<PropertyName*>      HandlePropertyName;
typedef JS::Handle<ArrayObject*>       HandleArrayObject;
typedef JS::Handle<PlainObject*>       HandlePlainObject;
typedef JS::Handle<ScriptSourceObject*> HandleScriptSource;

typedef JS::MutableHandle<Shape*>      MutableHandleShape;
typedef JS::MutableHandle<JSAtom*>     MutableHandleAtom;
typedef JS::MutableHandle<NativeObject*> MutableHandleNativeObject;

typedef JS::Rooted<NativeObject*>      RootedNativeObject;
typedef JS::Rooted<Shape*>             RootedShape;
typedef JS::Rooted<ObjectGroup*>       RootedObjectGroup;
typedef JS::Rooted<JSAtom*>            RootedAtom;
typedef JS::Rooted<JSLinearString*>    RootedLinearString;
typedef JS::Rooted<PropertyName*>      RootedPropertyName;
typedef JS::Rooted<ArrayObject*>       RootedArrayObject;
typedef JS::Rooted<GlobalObject*>      RootedGlobalObject;
typedef JS::Rooted<PlainObject*>       RootedPlainObject;
typedef JS::Rooted<ScriptSourceObject*> RootedScriptSource;

} /* namespace js */

#endif /* gc_Rooting_h */
