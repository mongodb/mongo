/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS boolean interface. */

#ifndef builtin_Boolean_h
#define builtin_Boolean_h

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;

namespace js {
class PropertyName;

extern PropertyName* BooleanToString(JSContext* cx, bool b);

}  // namespace js

#endif /* builtin_Boolean_h */
