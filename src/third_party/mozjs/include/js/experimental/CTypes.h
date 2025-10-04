/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_CTypes_h
#define js_experimental_CTypes_h

#include "mozilla/Attributes.h"  // MOZ_RAII

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

#ifdef JS_HAS_CTYPES

/**
 * Initialize the 'ctypes' object on a global variable 'obj'. The 'ctypes'
 * object will be sealed.
 */
extern JS_PUBLIC_API bool InitCTypesClass(JSContext* cx,
                                          Handle<JSObject*> global);

#endif  // JS_HAS_CTYPES

/**
 * The type of ctypes activity that is occurring.
 */
enum class CTypesActivityType {
  BeginCall,
  EndCall,
  BeginCallback,
  EndCallback,
};

/**
 * The signature of a function invoked at the leading or trailing edge of ctypes
 * activity.
 */
using CTypesActivityCallback = void (*)(JSContext*, CTypesActivityType);

/**
 * Sets a callback that is run whenever js-ctypes is about to be used when
 * calling into C.
 */
extern JS_PUBLIC_API void SetCTypesActivityCallback(JSContext* cx,
                                                    CTypesActivityCallback cb);

class MOZ_RAII JS_PUBLIC_API AutoCTypesActivityCallback {
 private:
  JSContext* cx;
  CTypesActivityCallback callback;
  CTypesActivityType endType;

 public:
  AutoCTypesActivityCallback(JSContext* cx, CTypesActivityType beginType,
                             CTypesActivityType endType);

  ~AutoCTypesActivityCallback() { DoEndCallback(); }

  void DoEndCallback() {
    if (callback) {
      callback(cx, endType);
      callback = nullptr;
    }
  }
};

#ifdef JS_HAS_CTYPES

/**
 * Convert a unicode string 'source' of length 'slen' to the platform native
 * charset, returning a null-terminated string allocated with JS_malloc. On
 * failure, this function should report an error.
 */
using CTypesUnicodeToNativeFun = char* (*)(JSContext*, const char16_t*, size_t);

/**
 * Set of function pointers that ctypes can use for various internal functions.
 * See JS::SetCTypesCallbacks below. Providing nullptr for a function is safe
 * and will result in the applicable ctypes functionality not being available.
 */
struct CTypesCallbacks {
  CTypesUnicodeToNativeFun unicodeToNative;
};

/**
 * Set the callbacks on the provided 'ctypesObj' object. 'callbacks' should be a
 * pointer to static data that exists for the lifetime of 'ctypesObj', but it
 * may safely be altered after calling this function and without having
 * to call this function again.
 */
extern JS_PUBLIC_API void SetCTypesCallbacks(JSObject* ctypesObj,
                                             const CTypesCallbacks* callbacks);

#endif  // JS_HAS_CTYPES

}  // namespace JS

#endif  // js_experimental_CTypes_h
