/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This file contains public type declarations that are used *frequently*.  If
// it doesn't occur at least 10 times in Gecko, it probably shouldn't be in
// here.
//
// It includes only:
// - forward declarations of structs and classes;
// - typedefs;
// - enums (maybe).
// It does *not* contain any struct or class definitions.

#ifndef js_TypeDecls_h
#define js_TypeDecls_h

#include <stddef.h>
#include <stdint.h>

#include "js-config.h"

typedef uint8_t jsbytecode;

class JSAtom;
struct JSCompartment;
struct JSContext;
class JSFunction;
class JSObject;
struct JSRuntime;
class JSScript;
class JSString;
class JSAddonId;
struct JSFreeOp;

struct jsid;

namespace JS {

typedef unsigned char Latin1Char;

class Symbol;
class Value;
class Realm;
struct Runtime;
struct Zone;

template <typename T> class Handle;
template <typename T> class MutableHandle;
template <typename T> class Rooted;
template <typename T> class PersistentRooted;

typedef Handle<JSFunction*> HandleFunction;
typedef Handle<jsid>        HandleId;
typedef Handle<JSObject*>   HandleObject;
typedef Handle<JSScript*>   HandleScript;
typedef Handle<JSString*>   HandleString;
typedef Handle<JS::Symbol*> HandleSymbol;
typedef Handle<Value>       HandleValue;

typedef MutableHandle<JSFunction*> MutableHandleFunction;
typedef MutableHandle<jsid>        MutableHandleId;
typedef MutableHandle<JSObject*>   MutableHandleObject;
typedef MutableHandle<JSScript*>   MutableHandleScript;
typedef MutableHandle<JSString*>   MutableHandleString;
typedef MutableHandle<JS::Symbol*> MutableHandleSymbol;
typedef MutableHandle<Value>       MutableHandleValue;

typedef Rooted<JSObject*>       RootedObject;
typedef Rooted<JSFunction*>     RootedFunction;
typedef Rooted<JSScript*>       RootedScript;
typedef Rooted<JSString*>       RootedString;
typedef Rooted<JS::Symbol*>     RootedSymbol;
typedef Rooted<jsid>            RootedId;
typedef Rooted<JS::Value>       RootedValue;

typedef PersistentRooted<JSFunction*> PersistentRootedFunction;
typedef PersistentRooted<jsid>        PersistentRootedId;
typedef PersistentRooted<JSObject*>   PersistentRootedObject;
typedef PersistentRooted<JSScript*>   PersistentRootedScript;
typedef PersistentRooted<JSString*>   PersistentRootedString;
typedef PersistentRooted<JS::Symbol*> PersistentRootedSymbol;
typedef PersistentRooted<Value>       PersistentRootedValue;

} // namespace JS

#endif /* js_TypeDecls_h */
