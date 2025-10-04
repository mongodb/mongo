/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* SpiderMonkey buildId-related functionality. */

#include "js/BuildId.h"  // JS::BuildIdCharVector, JS::BuildIdOp, JS::GetOptimizedEncodingBuildId, JS::SetProcessBuildIdOp

#include "mozilla/Atomics.h"  // mozilla::Atomic

#include "jstypes.h"  // JS_PUBLIC_API

#include "vm/Runtime.h"       // js::GetBuildId
#include "wasm/WasmModule.h"  // js::wasm::GetOptimizedEncodingBuildId

mozilla::Atomic<JS::BuildIdOp> js::GetBuildId;

JS_PUBLIC_API void JS::SetProcessBuildIdOp(JS::BuildIdOp buildIdOp) {
  js::GetBuildId = buildIdOp;
}

JS_PUBLIC_API bool JS::GetOptimizedEncodingBuildId(
    JS::BuildIdCharVector* buildId) {
  return js::wasm::GetOptimizedEncodingBuildId(buildId);
}
