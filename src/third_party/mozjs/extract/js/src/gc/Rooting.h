/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Rooting_h
#define gc_Rooting_h

#include "gc/Allocator.h"
#include "gc/Policy.h"
#include "js/GCVector.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

class JSLinearString;

namespace js {

class PropertyName;
class NativeObject;
class ArrayObject;
class GlobalObject;
class PlainObject;
class ScriptSourceObject;
class SavedFrame;
class Shape;
class DebuggerArguments;
class DebuggerEnvironment;
class DebuggerFrame;
class DebuggerObject;
class DebuggerScript;
class DebuggerSource;
class Scope;
class ModuleObject;

// These are internal counterparts to the public types such as HandleObject.

using HandleNativeObject = JS::Handle<NativeObject*>;
using HandleShape = JS::Handle<Shape*>;
using HandleAtom = JS::Handle<JSAtom*>;
using HandleLinearString = JS::Handle<JSLinearString*>;
using HandlePropertyName = JS::Handle<PropertyName*>;
using HandleArrayObject = JS::Handle<ArrayObject*>;
using HandlePlainObject = JS::Handle<PlainObject*>;
using HandleSavedFrame = JS::Handle<SavedFrame*>;
using HandleScriptSourceObject = JS::Handle<ScriptSourceObject*>;
using HandleDebuggerArguments = JS::Handle<DebuggerArguments*>;
using HandleDebuggerEnvironment = JS::Handle<DebuggerEnvironment*>;
using HandleDebuggerFrame = JS::Handle<DebuggerFrame*>;
using HandleDebuggerObject = JS::Handle<DebuggerObject*>;
using HandleDebuggerScript = JS::Handle<DebuggerScript*>;
using HandleDebuggerSource = JS::Handle<DebuggerSource*>;
using HandleScope = JS::Handle<Scope*>;
using HandleModuleObject = JS::Handle<ModuleObject*>;

using MutableHandleShape = JS::MutableHandle<Shape*>;
using MutableHandleAtom = JS::MutableHandle<JSAtom*>;
using MutableHandleNativeObject = JS::MutableHandle<NativeObject*>;
using MutableHandlePlainObject = JS::MutableHandle<PlainObject*>;
using MutableHandleSavedFrame = JS::MutableHandle<SavedFrame*>;
using MutableHandleDebuggerArguments = JS::MutableHandle<DebuggerArguments*>;
using MutableHandleDebuggerEnvironment =
    JS::MutableHandle<DebuggerEnvironment*>;
using MutableHandleDebuggerFrame = JS::MutableHandle<DebuggerFrame*>;
using MutableHandleDebuggerObject = JS::MutableHandle<DebuggerObject*>;
using MutableHandleDebuggerScript = JS::MutableHandle<DebuggerScript*>;
using MutableHandleDebuggerSource = JS::MutableHandle<DebuggerSource*>;
using MutableHandleScope = JS::MutableHandle<Scope*>;
using MutableHandleModuleObject = JS::MutableHandle<ModuleObject*>;
using MutableHandleArrayObject = JS::MutableHandle<ArrayObject*>;

using RootedNativeObject = JS::Rooted<NativeObject*>;
using RootedShape = JS::Rooted<Shape*>;
using RootedAtom = JS::Rooted<JSAtom*>;
using RootedLinearString = JS::Rooted<JSLinearString*>;
using RootedPropertyName = JS::Rooted<PropertyName*>;
using RootedArrayObject = JS::Rooted<ArrayObject*>;
using RootedGlobalObject = JS::Rooted<GlobalObject*>;
using RootedPlainObject = JS::Rooted<PlainObject*>;
using RootedSavedFrame = JS::Rooted<SavedFrame*>;
using RootedScriptSourceObject = JS::Rooted<ScriptSourceObject*>;
using RootedDebuggerArguments = JS::Rooted<DebuggerArguments*>;
using RootedDebuggerEnvironment = JS::Rooted<DebuggerEnvironment*>;
using RootedDebuggerFrame = JS::Rooted<DebuggerFrame*>;
using RootedDebuggerObject = JS::Rooted<DebuggerObject*>;
using RootedDebuggerScript = JS::Rooted<DebuggerScript*>;
using RootedDebuggerSource = JS::Rooted<DebuggerSource*>;
using RootedScope = JS::Rooted<Scope*>;
using RootedModuleObject = JS::Rooted<ModuleObject*>;

using FunctionVector = JS::GCVector<JSFunction*>;
using PropertyNameVector = JS::GCVector<PropertyName*>;
using ShapeVector = JS::GCVector<Shape*>;
using StringVector = JS::GCVector<JSString*>;

} /* namespace js */

#endif /* gc_Rooting_h */
