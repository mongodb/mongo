/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Special allocation functions for DOM binding code.
 *
 */

#ifndef js_experimental_BindingAllocs_h
#define js_experimental_BindingAllocs_h

#include "jstypes.h"

/**
 * Unlike JS_NewObject, JS_NewObjectWithGivenProtoAndUseAllocSite does not
 * compute a default proto. If proto is nullptr, the JS object will have `null`
 * as [[Prototype]].
 *
 * If the JIT code has set an allocation site, this allocation will consume that
 * allocation site. This only happens for DOM calls with Object return type.
 *
 * Must be called with a non-null clasp.
 *
 */
extern JS_PUBLIC_API JSObject* JS_NewObjectWithGivenProtoAndUseAllocSite(
    JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

#endif  // js_experimental_BindingAllocs_h
