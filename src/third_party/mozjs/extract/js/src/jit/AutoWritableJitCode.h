/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AutoWritableJitCode_h
#define jit_AutoWritableJitCode_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TimeStamp.h"

#include <stddef.h>

#include "jit/ExecutableAllocator.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/ProcessExecutableMemory.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"

namespace js::jit {

// This class ensures JIT code is executable on its destruction. Creators
// must call makeWritable(), and not attempt to write to the buffer if it fails.
//
// AutoWritableJitCodeFallible may only fail to make code writable; it cannot
// fail to make JIT code executable (because the creating code has no chance to
// recover from a failed destructor).
class MOZ_RAII AutoWritableJitCodeFallible {
  JSRuntime* rt_;
  void* addr_;
  size_t size_;
  AutoMarkJitCodeWritableForThread writableForThread_;

 public:
  AutoWritableJitCodeFallible(JSRuntime* rt, void* addr, size_t size)
      : rt_(rt), addr_(addr), size_(size) {
    rt_->toggleAutoWritableJitCodeActive(true);
  }

  AutoWritableJitCodeFallible(void* addr, size_t size)
      : AutoWritableJitCodeFallible(TlsContext.get()->runtime(), addr, size) {}

  explicit AutoWritableJitCodeFallible(JitCode* code)
      : AutoWritableJitCodeFallible(code->runtimeFromMainThread(), code->raw(),
                                    code->bufferSize()) {}

  [[nodiscard]] bool makeWritable() {
    return ExecutableAllocator::makeWritable(addr_, size_);
  }

  ~AutoWritableJitCodeFallible() {
    // Taking TimeStamps frequently can be expensive, and there's no point
    // measuring this if write protection is disabled.
    const bool measuringTime = JitOptions.writeProtectCode;
    const mozilla::TimeStamp startTime =
        measuringTime ? mozilla::TimeStamp::Now() : mozilla::TimeStamp();
    auto timer = mozilla::MakeScopeExit([&] {
      if (measuringTime) {
        if (Realm* realm = rt_->mainContextFromOwnThread()->realm()) {
          realm->timers.protectTime += mozilla::TimeStamp::Now() - startTime;
        }
      }
    });

    if (!ExecutableAllocator::makeExecutableAndFlushICache(addr_, size_)) {
      MOZ_CRASH();
    }
    rt_->toggleAutoWritableJitCodeActive(false);
  }
};

// Infallible variant of AutoWritableJitCodeFallible, ensures writable during
// construction
class MOZ_RAII AutoWritableJitCode : private AutoWritableJitCodeFallible {
 public:
  AutoWritableJitCode(JSRuntime* rt, void* addr, size_t size)
      : AutoWritableJitCodeFallible(rt, addr, size) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!makeWritable()) {
      oomUnsafe.crash("Failed to mmap. Likely no mappings available.");
    }
  }

  AutoWritableJitCode(void* addr, size_t size)
      : AutoWritableJitCode(TlsContext.get()->runtime(), addr, size) {}

  explicit AutoWritableJitCode(JitCode* code)
      : AutoWritableJitCode(code->runtimeFromMainThread(), code->raw(),
                            code->bufferSize()) {}
};

}  // namespace js::jit

#endif /* jit_AutoWritableJitCode_h */
