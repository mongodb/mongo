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
#include "jit/Ion.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "jit/PerfSpewer.h"
#include "js/HeapAPI.h"
#include "vm/JSContext.h"

#ifdef JS_CODEGEN_ARM64
#  include "jit/arm64/vixl/Cpu-vixl.h"
#endif

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

void jit::SetJitContext(JitContext* ctx) {
  MOZ_ASSERT(!TlsJitContext.get());
  TlsJitContext.set(ctx);
}

JitContext* jit::GetJitContext() {
  MOZ_ASSERT(CurrentJitContext());
  return CurrentJitContext();
}

JitContext* jit::MaybeGetJitContext() { return CurrentJitContext(); }

JitContext::JitContext(CompileRuntime* rt) : runtime(rt) {
  MOZ_ASSERT(rt);
  SetJitContext(this);
}

JitContext::JitContext(JSContext* cx)
    : cx(cx), runtime(CompileRuntime::get(cx->runtime())) {
  SetJitContext(this);
}

JitContext::JitContext() {
#ifdef DEBUG
  isCompilingWasm_ = true;
#endif
  SetJitContext(this);
}

JitContext::~JitContext() {
  MOZ_ASSERT(TlsJitContext.get() == this);
  TlsJitContext.set(nullptr);
}

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

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // Compute flags.
  js::jit::CPUInfo::ComputeFlags();
#endif

#if defined(JS_CODEGEN_ARM)
  InitARMFlags();
#endif

#ifdef JS_CODEGEN_ARM64
  // Initialize instruction cache flushing.
  vixl::CPU::SetUp();
#endif

#ifndef JS_CODEGEN_NONE
  MOZ_ASSERT(js::jit::CPUFlagsHaveBeenComputed());
#endif

  // Note: jit flags need to be initialized after the InitARMFlags call above.
  // This is the final point where we can set disableJitBackend = true, before
  // we use this flag below with the HasJitBackend call.
  if (!MacroAssembler::SupportsFloatingPoint()) {
    JitOptions.disableJitBackend = true;
  }
  JitOptions.supportsUnalignedAccesses =
      MacroAssembler::SupportsUnalignedAccesses();

  if (HasJitBackend()) {
    if (!InitProcessExecutableMemory()) {
      return false;
    }
  }

  PerfSpewer::Init();
  return true;
}

void jit::ShutdownJit() {
  if (HasJitBackend() && !JSRuntime::hasLiveRuntimes()) {
    ReleaseProcessExecutableMemory();
  }
}

bool jit::JitSupportsWasmSimd() {
#if defined(ENABLE_WASM_SIMD)
  return js::jit::MacroAssembler::SupportsWasmSimd();
#else
  return false;
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
