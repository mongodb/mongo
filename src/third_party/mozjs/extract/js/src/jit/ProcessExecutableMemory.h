/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ProcessExecutableMemory_h
#define jit_ProcessExecutableMemory_h

#include "util/Poison.h"

namespace js {
namespace jit {

// Limit on the number of bytes of executable memory to prevent JIT spraying
// attacks.
#if JS_BITS_PER_WORD == 32
static const size_t MaxCodeBytesPerProcess = 140 * 1024 * 1024;
#else
// This is the largest number which satisfies various alignment static
// asserts that is <= INT32_MAX. The INT32_MAX limit is required for making a
// single call to RtlInstallFunctionTableCallback(). (This limit could be
// relaxed in the future by making multiple calls.)
static const size_t MaxCodeBytesPerProcess = 2044 * 1024 * 1024;
#endif

// Limit on the number of bytes of code memory per buffer.  This limit comes
// about because we encode an unresolved relative unconditional branch during
// assembly as a branch instruction that carries the absolute offset of the next
// branch instruction in the chain of branches that all reference the same
// unresolved label.  For this architecture to work, no branch instruction may
// lie at an offset greater than the maximum forward branch distance.  This is
// true on both ARM and ARM64.
//
// Notably, even though we know that the offsets thus encoded are always
// positive offsets, we use only the positive part of the signed range of the
// branch offset.
//
// On ARM-32, we are limited by BOffImm::IsInRange(), which checks that the
// offset is no greater than 2^25-4 in the offset's 26-bit signed field.
//
// On ARM-64, we are limited by Instruction::ImmBranchMaxForwardOffset(), which
// checks that the offset is no greater than 2^27-4 in the offset's 28-bit
// signed field.
//
// On MIPS, there are no limitations because the assembler has to implement
// jump chaining to be effective at all (jump offsets are quite small).
//
// On x86 and x64, there are no limitations here because the assembler
// MOZ_CRASHes if the 32-bit offset is exceeded.

#if defined(JS_CODEGEN_ARM)
static const size_t MaxCodeBytesPerBuffer = (1 << 25) - 4;
#elif defined(JS_CODEGEN_ARM64)
static const size_t MaxCodeBytesPerBuffer = (1 << 27) - 4;
#else
static const size_t MaxCodeBytesPerBuffer = MaxCodeBytesPerProcess;
#endif

// Executable code is allocated in 64K chunks. ExecutableAllocator uses pools
// that are at least this big. Code we allocate does not necessarily have 64K
// alignment though.
static const size_t ExecutableCodePageSize = 64 * 1024;

enum class ProtectionSetting {
  Protected,  // Not readable, writable, or executable.
  Writable,
  Executable,
};

/// Whether the instruction cache must be flushed

enum class MustFlushICache { No, Yes };

[[nodiscard]] extern bool ReprotectRegion(void* start, size_t size,
                                          ProtectionSetting protection,
                                          MustFlushICache flushICache);

// Functions called at process start-up/shutdown to initialize/release the
// executable memory region.
[[nodiscard]] extern bool InitProcessExecutableMemory();
extern void ReleaseProcessExecutableMemory();

// Allocate/deallocate executable pages.
extern void* AllocateExecutableMemory(size_t bytes,
                                      ProtectionSetting protection,
                                      MemCheckKind checkKind);
extern void DeallocateExecutableMemory(void* addr, size_t bytes);

// Returns true if we can allocate a few more MB of executable code without
// hitting our code limit. This function can be used to stop compiling things
// that are optional (like Baseline and Ion code) when we're about to reach the
// limit, so we are less likely to OOM or crash. Note that the limit is
// per-process, so other threads can also allocate code after we call this
// function.
extern bool CanLikelyAllocateMoreExecutableMemory();

// Returns a rough guess of how much executable memory remains available,
// rounded down to MB limit.  Note this can fluctuate as other threads within
// the process allocate executable memory.
extern size_t LikelyAvailableExecutableMemory();

// Returns whether |p| is stored in the executable code buffer.
extern bool AddressIsInExecutableMemory(const void* p);

}  // namespace jit
}  // namespace js

#endif  // jit_ProcessExecutableMemory_h
