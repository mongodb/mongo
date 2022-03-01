/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_Intl_h
#define js_experimental_Intl_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

/**
 * Create and add the Intl.MozDateTimeFormat constructor function to the
 * provided object.
 *
 * This custom date/time formatter constructor gives users the ability to
 * specify a custom format pattern. This pattern is passed *directly* to ICU
 * with NO SYNTAX PARSING OR VALIDATION WHATSOEVER.  ICU appears to have a
 * modicum of testing of this, and it won't fall over completely if passed bad
 * input.  But the current behavior is entirely under-specified and emphatically
 * not shippable on the web, and it *must* be fixed before this functionality
 * can be exposed in the real world.  (There are also some questions about
 * whether the format exposed here is the *right* one to standardize, that will
 * also need to be resolved to ship this.)
 *
 * If JS was built without JS_HAS_INTL_API, this function will throw an
 * exception.
 */
extern JS_PUBLIC_API bool AddMozDateTimeFormatConstructor(
    JSContext* cx, Handle<JSObject*> intl);

/**
 * Create and add the Intl.MozDisplayNames constructor function to the
 * provided object.  This constructor acts like the standard |Intl.DisplayNames|
 * but accepts certain additional syntax that isn't standardized to the point of
 * being shippable.
 *
 * If JS was built without JS_HAS_INTL_API, this function will throw an
 * exception.
 */
extern JS_PUBLIC_API bool AddMozDisplayNamesConstructor(JSContext* cx,
                                                        Handle<JSObject*> intl);

}  // namespace JS

#endif  // js_experimental_Intl_h
