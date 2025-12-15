/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TestingUtility.h"

#include <stdint.h>  // uint32_t

#include "jsapi.h"  // JS_NewPlainObject, JS_WrapValue

#include "frontend/CompilationStencil.h"  // js::frontend::CompilationStencil
#include "js/CharacterEncoding.h"         // JS_EncodeStringToUTF8
#include "js/ColumnNumber.h"              // JS::ColumnNumberOneOrigin
#include "js/CompileOptions.h"            // JS::CompileOptions
#include "js/Conversions.h"  // JS::ToBoolean, JS::ToString, JS::ToUint32, JS::ToInt32
#include "js/PropertyAndElement.h"  // JS_GetProperty, JS_DefineProperty
#include "js/PropertyDescriptor.h"  // JSPROP_ENUMERATE
#include "js/RealmOptions.h"        // JS::RealmBehaviors
#include "js/RootingAPI.h"          // JS::Rooted, JS::Handle
#include "js/Utility.h"             // JS::UniqueChars
#include "js/Value.h"               // JS::Value, JS::StringValue
#include "vm/JSContext.h"           // JS::ReportUsageErrorASCII
#include "vm/JSScript.h"
#include "vm/Realm.h"  // JS::Realm

bool js::ParseCompileOptions(JSContext* cx, JS::CompileOptions& options,
                             JS::Handle<JSObject*> opts,
                             JS::UniqueChars* fileNameBytes) {
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
      *fileNameBytes = JS_EncodeStringToUTF8(cx, s);
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
    if (c < 1) {
      c = 1;
    }
    options.setColumn(JS::ColumnNumberOneOrigin(c));
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
  bool forceFullParseIsSet = !v.isUndefined();
  if (v.isBoolean() && v.toBoolean()) {
    options.setForceFullParse();
  }

  if (!JS_GetProperty(cx, opts, "eagerDelazificationStrategy", &v)) {
    return false;
  }
  if (forceFullParseIsSet && !v.isUndefined()) {
    JS_ReportErrorASCII(
        cx, "forceFullParse and eagerDelazificationStrategy are both set.");
    return false;
  }
  if (v.isString()) {
    s = JS::ToString(cx, v);
    if (!s) {
      return false;
    }

    JSLinearString* str = JS_EnsureLinearString(cx, s);
    if (!str) {
      return false;
    }

    bool found = false;
    JS::DelazificationOption strategy = JS::DelazificationOption::OnDemandOnly;

#define MATCH_AND_SET_STRATEGY_(NAME)                       \
  if (!found && JS_LinearStringEqualsLiteral(str, #NAME)) { \
    strategy = JS::DelazificationOption::NAME;              \
    found = true;                                           \
  }

    FOREACH_DELAZIFICATION_STRATEGY(MATCH_AND_SET_STRATEGY_);
#undef MATCH_AND_SET_STRATEGY_
#undef FOR_STRATEGY_NAMES

    if (!found) {
      JS_ReportErrorASCII(cx,
                          "eagerDelazificationStrategy does not match any "
                          "DelazificationOption.");
      return false;
    }
    options.setEagerDelazificationStrategy(strategy);
  }

  return true;
}

bool js::ParseSourceOptions(JSContext* cx, JS::Handle<JSObject*> opts,
                            JS::MutableHandle<JSString*> displayURL,
                            JS::MutableHandle<JSString*> sourceMapURL) {
  JS::Rooted<JS::Value> v(cx);

  if (!JS_GetProperty(cx, opts, "displayURL", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    displayURL.set(ToString(cx, v));
    if (!displayURL) {
      return false;
    }
  }

  if (!JS_GetProperty(cx, opts, "sourceMapURL", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    sourceMapURL.set(ToString(cx, v));
    if (!sourceMapURL) {
      return false;
    }
  }

  return true;
}

bool js::SetSourceOptions(JSContext* cx, FrontendContext* fc,
                          ScriptSource* source,
                          JS::Handle<JSString*> displayURL,
                          JS::Handle<JSString*> sourceMapURL) {
  if (displayURL && !source->hasDisplayURL()) {
    JS::UniqueTwoByteChars chars = JS_CopyStringCharsZ(cx, displayURL);
    if (!chars) {
      return false;
    }
    if (!source->setDisplayURL(fc, std::move(chars))) {
      return false;
    }
  }
  if (sourceMapURL && !source->hasSourceMapURL()) {
    JS::UniqueTwoByteChars chars = JS_CopyStringCharsZ(cx, sourceMapURL);
    if (!chars) {
      return false;
    }
    if (!source->setSourceMapURL(fc, std::move(chars))) {
      return false;
    }
  }

  return true;
}

JSObject* js::CreateScriptPrivate(JSContext* cx,
                                  JS::Handle<JSString*> path /* = nullptr */) {
  JS::Rooted<JSObject*> info(cx, JS_NewPlainObject(cx));
  if (!info) {
    return nullptr;
  }

  if (path) {
    JS::Rooted<JS::Value> pathValue(cx, JS::StringValue(path));
    if (!JS_DefineProperty(cx, info, "path", pathValue, JSPROP_ENUMERATE)) {
      return nullptr;
    }
  }

  return info;
}

bool js::ParseDebugMetadata(JSContext* cx, JS::Handle<JSObject*> opts,
                            JS::MutableHandle<JS::Value> privateValue,
                            JS::MutableHandle<JSString*> elementAttributeName) {
  JS::Rooted<JS::Value> v(cx);
  JS::Rooted<JSString*> s(cx);

  if (!JS_GetProperty(cx, opts, "element", &v)) {
    return false;
  }
  if (v.isObject()) {
    JS::Rooted<JSObject*> infoObject(cx, CreateScriptPrivate(cx));
    if (!infoObject) {
      return false;
    }
    JS::Rooted<JS::Value> elementValue(cx, v);
    if (!JS_WrapValue(cx, &elementValue)) {
      return false;
    }
    if (!JS_DefineProperty(cx, infoObject, "element", elementValue, 0)) {
      return false;
    }
    privateValue.set(JS::ObjectValue(*infoObject));
  }

  if (!JS_GetProperty(cx, opts, "elementAttributeName", &v)) {
    return false;
  }
  if (!v.isUndefined()) {
    s = ToString(cx, v);
    if (!s) {
      return false;
    }
    elementAttributeName.set(s);
  }

  return true;
}

JS::UniqueChars js::StringToLocale(JSContext* cx, JS::Handle<JSObject*> callee,
                                   JS::Handle<JSString*> str_) {
  Rooted<JSLinearString*> str(cx, str_->ensureLinear(cx));
  if (!str) {
    return nullptr;
  }

  if (!StringIsAscii(str)) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument contains non-ASCII characters");
    return nullptr;
  }

  UniqueChars locale = JS_EncodeStringToASCII(cx, str);
  if (!locale) {
    return nullptr;
  }

  bool containsOnlyValidBCP47Characters =
      mozilla::IsAsciiAlpha(locale[0]) &&
      std::all_of(locale.get(), locale.get() + str->length(), [](auto c) {
        return mozilla::IsAsciiAlphanumeric(c) || c == '-';
      });

  if (!containsOnlyValidBCP47Characters) {
    ReportUsageErrorASCII(cx, callee,
                          "First argument should be a BCP47 language tag");
    return nullptr;
  }

  return locale;
}

bool js::ValidateLazinessOfStencilAndGlobal(JSContext* cx,
                                            const JS::Stencil* stencil) {
  if (cx->realm()->behaviors().discardSource() && stencil->canLazilyParse()) {
    JS_ReportErrorASCII(cx,
                        "Stencil compiled with with lazy parse option cannot "
                        "be used in a realm with discardSource");
    return false;
  }

  return true;
}

bool js::ValidateModuleCompileOptions(JSContext* cx,
                                      JS::CompileOptions& options) {
  if (options.lineno == 0) {
    JS_ReportErrorASCII(cx, "Module cannot be compiled with lineNumber == 0");
    return false;
  }

  if (!options.filename()) {
    JS_ReportErrorASCII(cx, "Module should have filename");
    return false;
  }

  return true;
}
