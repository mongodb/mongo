/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Higher-order macros enumerating public untagged and tagged GC pointer types.
 */

#ifndef GCTypeMacros_h
#define GCTypeMacros_h

#include "jstypes.h"  // JS_PUBLIC_API

class JS_PUBLIC_API JSAtom;
class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;
class JS_PUBLIC_API JSScript;
class JS_PUBLIC_API JSString;

namespace JS {
class JS_PUBLIC_API BigInt;
class JS_PUBLIC_API PropertyKey;
class JS_PUBLIC_API Symbol;
class JS_PUBLIC_API Value;
}  // namespace JS

// Expand the given macro D for each public GC pointer.
#define JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(D) \
  D(JS::BigInt*)                              \
  D(JS::Symbol*)                              \
  D(JSAtom*)                                  \
  D(JSFunction*)                              \
  D(JSLinearString*)                          \
  D(JSObject*)                                \
  D(JSScript*)                                \
  D(JSString*)

// Expand the given macro D for each public tagged GC pointer type.
#define JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(D) \
  D(JS::Value)                                       \
  D(JS::PropertyKey)  // i.e. jsid

#endif  // GCTypeMacros_h
