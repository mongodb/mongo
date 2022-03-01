/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Reflect_h
#define builtin_Reflect_h

#include "vm/JSObject.h"

namespace js {

extern const JSClass ReflectClass;

}  // namespace js

namespace js {

[[nodiscard]] extern bool Reflect_getPrototypeOf(JSContext* cx, unsigned argc,
                                                 Value* vp);

[[nodiscard]] extern bool Reflect_isExtensible(JSContext* cx, unsigned argc,
                                               Value* vp);

[[nodiscard]] extern bool Reflect_ownKeys(JSContext* cx, unsigned argc,
                                          Value* vp);

}  // namespace js

#endif /* builtin_Reflect_h */
