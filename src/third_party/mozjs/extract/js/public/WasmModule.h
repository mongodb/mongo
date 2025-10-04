/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WasmModule_h
#define js_WasmModule_h

#include "mozilla/RefPtr.h"  // RefPtr

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/RefCounted.h"  // AtomicRefCounted
#include "js/TypeDecls.h"   // HandleObject

namespace JS {

/**
 * The WasmModule interface allows the embedding to hold a reference to the
 * underying C++ implementation of a JS WebAssembly.Module object for purposes
 * of efficient postMessage() and (de)serialization from a random thread.
 *
 * In particular, this allows postMessage() of a WebAssembly.Module:
 * GetWasmModule() is called when making a structured clone of a payload
 * containing a WebAssembly.Module object. The structured clone buffer holds a
 * refcount of the JS::WasmModule until createObject() is called in the target
 * agent's JSContext. The new WebAssembly.Module object continues to hold the
 * JS::WasmModule and thus the final reference of a JS::WasmModule may be
 * dropped from any thread and so the virtual destructor (and all internal
 * methods of the C++ module) must be thread-safe.
 */

struct WasmModule : js::AtomicRefCounted<WasmModule> {
  virtual ~WasmModule() = default;
  virtual JSObject* createObject(JSContext* cx) const = 0;
  virtual JSObject* createObjectForAsmJS(JSContext* cx) const = 0;
};

extern JS_PUBLIC_API bool IsWasmModuleObject(HandleObject obj);

extern JS_PUBLIC_API RefPtr<WasmModule> GetWasmModule(HandleObject obj);

}  // namespace JS

#endif /* js_WasmModule_h */
