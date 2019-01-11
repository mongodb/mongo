/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Rooting_h
#define gc_Rooting_h

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
class ObjectGroup;
class DebuggerArguments;
class DebuggerEnvironment;
class DebuggerFrame;
class DebuggerObject;
class Scope;

// These are internal counterparts to the public types such as HandleObject.

typedef JS::Handle<NativeObject*>           HandleNativeObject;
typedef JS::Handle<Shape*>                  HandleShape;
typedef JS::Handle<ObjectGroup*>            HandleObjectGroup;
typedef JS::Handle<JSAtom*>                 HandleAtom;
typedef JS::Handle<JSLinearString*>         HandleLinearString;
typedef JS::Handle<PropertyName*>           HandlePropertyName;
typedef JS::Handle<ArrayObject*>            HandleArrayObject;
typedef JS::Handle<PlainObject*>            HandlePlainObject;
typedef JS::Handle<SavedFrame*>             HandleSavedFrame;
typedef JS::Handle<ScriptSourceObject*>     HandleScriptSource;
typedef JS::Handle<DebuggerArguments*>      HandleDebuggerArguments;
typedef JS::Handle<DebuggerEnvironment*>    HandleDebuggerEnvironment;
typedef JS::Handle<DebuggerFrame*>          HandleDebuggerFrame;
typedef JS::Handle<DebuggerObject*>         HandleDebuggerObject;
typedef JS::Handle<Scope*>                  HandleScope;

typedef JS::MutableHandle<Shape*>               MutableHandleShape;
typedef JS::MutableHandle<JSAtom*>              MutableHandleAtom;
typedef JS::MutableHandle<NativeObject*>        MutableHandleNativeObject;
typedef JS::MutableHandle<PlainObject*>         MutableHandlePlainObject;
typedef JS::MutableHandle<SavedFrame*>          MutableHandleSavedFrame;
typedef JS::MutableHandle<DebuggerArguments*>   MutableHandleDebuggerArguments;
typedef JS::MutableHandle<DebuggerEnvironment*> MutableHandleDebuggerEnvironment;
typedef JS::MutableHandle<DebuggerFrame*>       MutableHandleDebuggerFrame;
typedef JS::MutableHandle<DebuggerObject*>      MutableHandleDebuggerObject;
typedef JS::MutableHandle<Scope*>               MutableHandleScope;

typedef JS::Rooted<NativeObject*>           RootedNativeObject;
typedef JS::Rooted<Shape*>                  RootedShape;
typedef JS::Rooted<ObjectGroup*>            RootedObjectGroup;
typedef JS::Rooted<JSAtom*>                 RootedAtom;
typedef JS::Rooted<JSLinearString*>         RootedLinearString;
typedef JS::Rooted<PropertyName*>           RootedPropertyName;
typedef JS::Rooted<ArrayObject*>            RootedArrayObject;
typedef JS::Rooted<GlobalObject*>           RootedGlobalObject;
typedef JS::Rooted<PlainObject*>            RootedPlainObject;
typedef JS::Rooted<SavedFrame*>             RootedSavedFrame;
typedef JS::Rooted<ScriptSourceObject*>     RootedScriptSource;
typedef JS::Rooted<DebuggerArguments*>      RootedDebuggerArguments;
typedef JS::Rooted<DebuggerEnvironment*>    RootedDebuggerEnvironment;
typedef JS::Rooted<DebuggerFrame*>          RootedDebuggerFrame;
typedef JS::Rooted<DebuggerObject*>         RootedDebuggerObject;
typedef JS::Rooted<Scope*>                  RootedScope;

typedef JS::GCVector<JSFunction*>   FunctionVector;
typedef JS::GCVector<PropertyName*> PropertyNameVector;
typedef JS::GCVector<Shape*>        ShapeVector;
typedef JS::GCVector<JSString*>     StringVector;

/** Interface substitute for Rooted<T> which does not root the variable's memory. */
template <typename T>
class MOZ_RAII FakeRooted : public RootedBase<T, FakeRooted<T>>
{
  public:
    using ElementType = T;

    template <typename CX>
    explicit FakeRooted(CX* cx) : ptr(JS::GCPolicy<T>::initial()) {}

    template <typename CX>
    FakeRooted(CX* cx, T initial) : ptr(initial) {}

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(FakeRooted, T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr);

  private:
    T ptr;

    void set(const T& value) {
        ptr = value;
    }

    FakeRooted(const FakeRooted&) = delete;
};

/** Interface substitute for MutableHandle<T> which is not required to point to rooted memory. */
template <typename T>
class FakeMutableHandle : public js::MutableHandleBase<T, FakeMutableHandle<T>>
{
  public:
    using ElementType = T;

    MOZ_IMPLICIT FakeMutableHandle(T* t) {
        ptr = t;
    }

    MOZ_IMPLICIT FakeMutableHandle(FakeRooted<T>* root) {
        ptr = root->address();
    }

    void set(const T& v) {
        *ptr = v;
    }

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(*ptr);

  private:
    FakeMutableHandle() {}
    DELETE_ASSIGNMENT_OPS(FakeMutableHandle, T);

    T* ptr;
};

template <typename T> class MaybeRooted<T, NoGC>
{
  public:
    typedef const T& HandleType;
    typedef FakeRooted<T> RootType;
    typedef FakeMutableHandle<T> MutableHandleType;

    static JS::Handle<T> toHandle(HandleType v) {
        MOZ_CRASH("Bad conversion");
    }

    static JS::MutableHandle<T> toMutableHandle(MutableHandleType v) {
        MOZ_CRASH("Bad conversion");
    }

    template <typename T2>
    static inline T2* downcastHandle(HandleType v) {
        return &v->template as<T2>();
    }
};

} /* namespace js */

#endif /* gc_Rooting_h */
