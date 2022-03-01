/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TestingUtility.h"

#include <stdint.h>  // uint32_t

#include "jsapi.h"                 // JS_GetProperty
#include "js/CharacterEncoding.h"  // JS_EncodeStringToLatin1
#include "js/CompileOptions.h"     // JS::CompileOptions
#include "js/Conversions.h"  // JS::ToBoolean, JS::ToString, JS::ToUint32, JS::ToInt32
#include "js/RootingAPI.h"  // JS::Rooted, JS::Handle
#include "js/Utility.h"     // JS::UniqueChars
#include "js/Value.h"       // JS::Value
#include "vm/JSContext.h"   // JSContext
#include "vm/JSObject.h"    // JSObject
#include "vm/StringType.h"  // JSString

bool js::ParseCompileOptions(JSContext* cx, JS::CompileOptions& options,
                             JS::Handle<JSObject*> opts,
                             UniqueChars* fileNameBytes) {
  JS::Rooted<JS::Value> v(cx);
  JS::Rooted<JSString*> s(cx);

  if (!JS_GetProperty(cx, opts, "isRunOnce", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    options.setIsRunOnce(JS::ToBoolean(v));
  }

  if (!JS_GetProperty(cx, opts, "noScriptRval", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    options.setNoScriptRval(JS::ToBoolean(v));
  }

  if (!JS_GetProperty(cx, opts, "fileName", &v)) {
    return false;
  }
  if (v.isNull()) {
    options.setFile(nullptr);
  } else if (!v.isUndefined()) {
    s = JS::ToString(cx, v);
    if (!s) {
      return false;
    }
    if (fileNameBytes) {
      *fileNameBytes = JS_EncodeStringToLatin1(cx, s);
      if (!*fileNameBytes) {
        return false;
      }
      options.setFile(fileNameBytes->get());
    }
  }

  if (!JS_GetProperty(cx, opts, "skipFileNameValidation", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    options.setSkipFilenameValidation(JS::ToBoolean(v));
  }

  if (!JS_GetProperty(cx, opts, "lineNumber", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    uint32_t u;
    if (!JS::ToUint32(cx, v, &u)) {
      return false;
    }
    options.setLine(u);
  }

  if (!JS_GetProperty(cx, opts, "columnNumber", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    int32_t c;
    if (!JS::ToInt32(cx, v, &c)) {
      return false;
    }
    options.setColumn(c);
  }

  if (!JS_GetProperty(cx, opts, "sourceIsLazy", &v)) {
    return false;
  }
  if (v.isBoolean()) {
    options.setSourceIsLazy(v.toBoolean());
  }

  if (!JS_GetProperty(cx, opts, "forceFullParse", &v)) {
    return false;
  }
  if (v.isBoolean() && v.toBoolean()) {
    options.setForceFullParse();
  }

  return true;
}
