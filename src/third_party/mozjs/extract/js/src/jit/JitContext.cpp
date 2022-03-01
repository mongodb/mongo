/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitContext.h"

#include "mozilla/Assertions.h"
#include "mozilla/ThreadLocal.h"

#include <stdlib.h>

#include "jit/CacheIRSpewer.h"
#include "jit/CompileWrappers.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "jit/PerfSpewer.h"
#include "js/HeapAPI.h"
#include "vm/JSContext.h"

#if defined(ANDROID)
#  include <sys/system_properties.h>
#endif

using namespace js;
using namespace js::jit;

namespace js::jit {
class TempAllocator;
}

// Assert that JitCode is gc::Cell aligned.
static_assert(sizeof(JitCode) % gc::CellAlignBytes == 0);

static MOZ_THREAD_LOCAL(JitContext*) TlsJitContext;

static JitContext* CurrentJitContext() {
  if (!TlsJitContext.init()) {
    return nullptr;
  }
  return TlsJitContext.get();
}

void jit::SetJitContext(JitContext* ctx) { TlsJitContext.set(ctx); }

JitContext* jit::GetJitContext() {
  MOZ_ASSERT(CurrentJitContext());
  return CurrentJitContext();
}

JitContext* jit::MaybeGetJitContext() { return CurrentJitContext(); }

JitContext::JitContext(CompileRuntime* rt, CompileRealm* realm,
                       TempAllocator* temp)
    : prev_(CurrentJitContext()), realm_(realm), temp(temp), runtime(rt) {
  MOZ_ASSERT(rt);
  MOZ_ASSERT(realm);
  MOZ_ASSERT(temp);
  SetJitContext(this);
}

JitContext::JitContext(JSContext* cx, TempAllocator* temp)
    : prev_(CurrentJitContext()),
      realm_(CompileRealm::get(cx->realm())),
      cx(cx),
      temp(temp),
      runtime(CompileRuntime::get(cx->runtime())) {
  SetJitContext(this);
}

JitContext::JitContext(TempAllocator* temp)
    : prev_(CurrentJitContext()), temp(temp) {
#ifdef DEBUG
  isCompilingWasm_ = true;
#endif
  SetJitContext(this);
}

JitContext::JitContext() : JitContext(nullptr) {}

JitContext::~JitContext() { SetJitContext(prev_); }

bool jit::InitializeJit() {
  if (!TlsJitContext.init()) {
    return false;
  }

  CheckLogging();

#ifdef JS_CACHEIR_SPEW
  const char* env = getenv("CACHEIR_LOGS");
  if (env && env[0] && env[0] != '0') {
    CacheIRSpewer::singleton().init(env);
  }
#endif

#if defined(JS_CODEGEN_ARM)
  InitARMFlags();
#endif

  // Note: jit flags need to be initialized after the InitARMFlags call above.
  ComputeJitSupportFlags();

  CheckPerf();
  return true;
}

void jit::ComputeJitSupportFlags() {
  JitOptions.supportsFloatingPoint = MacroAssembler::SupportsFloatingPoint();
  JitOptions.supportsUnalignedAccesses =
      MacroAssembler::SupportsUnalignedAccesses();
}

bool jit::JitSupportsWasmSimd() {
#if defined(ENABLE_WASM_SIMD)
  return js::jit::MacroAssembler::SupportsWasmSimd();
#else
  MOZ_CRASH("Do not call");
#endif
}

bool jit::JitSupportsAtomics() {
#if defined(JS_CODEGEN_ARM)
  // Bug 1146902, bug 1077318: Enable Ion inlining of Atomics
  // operations on ARM only when the CPU has byte, halfword, and
  // doubleword load-exclusive and store-exclusive instructions,
  // until we can add support for systems that don't have those.
  return js::jit::HasLDSTREXBHD();
#else
  return true;
#endif
}
