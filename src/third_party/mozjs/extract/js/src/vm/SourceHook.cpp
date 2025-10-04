/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/experimental/SourceHook.h"

#include "mozilla/UniquePtr.h"  // mozilla::UniquePtr

#include <utility>  // std::move

#include "jstypes.h"  // JS_PUBLIC_API

#include "vm/JSContext.h"  // JSContext
#include "vm/Runtime.h"    // JSRuntime

JS_PUBLIC_API void js::SetSourceHook(JSContext* cx,
                                     mozilla::UniquePtr<SourceHook> hook) {
  cx->runtime()->sourceHook.ref() = std::move(hook);
}

JS_PUBLIC_API mozilla::UniquePtr<js::SourceHook> js::ForgetSourceHook(
    JSContext* cx) {
  return std::move(cx->runtime()->sourceHook.ref());
}
