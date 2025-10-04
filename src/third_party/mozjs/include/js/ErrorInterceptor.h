/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ErrorInterceptor_h
#define js_ErrorInterceptor_h

#include "jstypes.h"

#include "js/TypeDecls.h"

/**
 * Callback used to intercept JavaScript errors.
 */
struct JSErrorInterceptor {
  /**
   * This method is called whenever an error has been raised from JS code.
   *
   * This method MUST be infallible.
   */
  virtual void interceptError(JSContext* cx, JS::HandleValue error) = 0;
};

// Set a callback that will be called whenever an error
// is thrown in this runtime. This is designed as a mechanism
// for logging errors. Note that the VM makes no attempt to sanitize
// the contents of the error (so it may contain private data)
// or to sort out among errors (so it may not be the error you
// are interested in or for the component in which you are
// interested).
//
// If the callback sets a new error, this new error
// will replace the original error.
//
// May be `nullptr`.
// This is a no-op if built without NIGHTLY_BUILD.
extern JS_PUBLIC_API void JS_SetErrorInterceptorCallback(
    JSRuntime*, JSErrorInterceptor* callback);

// This returns nullptr if built without NIGHTLY_BUILD.
extern JS_PUBLIC_API JSErrorInterceptor* JS_GetErrorInterceptorCallback(
    JSRuntime*);

#endif  // js_ErrorInterceptor_h
