/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ToSource_h
#define vm_ToSource_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RootingAPI.h"  // JS::Handle
#include "js/Value.h"       // JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSString;

namespace js {

// Try to convert a value to its source expression, returning null after
// reporting an error, otherwise returning a new string.
extern JSString* ValueToSource(JSContext* cx, JS::Handle<JS::Value> v);

}  // namespace js

#endif  // vm_ToSource_h
