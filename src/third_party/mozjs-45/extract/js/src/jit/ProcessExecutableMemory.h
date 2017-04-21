/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ProcessExecutableMemory_h
#define jit_ProcessExecutableMemory_h

#include "mozilla/Attributes.h"

namespace js {
namespace jit {

// Executable code is allocated in 64K chunks. ExecutableAllocator uses pools
// that are at least this big. Code we allocate does not necessarily have 64K
// alignment though.
static const size_t ExecutableCodePageSize = 64 * 1024;

enum class ProtectionSetting {
    Protected, // Not readable, writable, or executable.
    Writable,
    Executable,
};

// Functions called at process start-up/shutdown to initialize/release the
// executable memory region.
extern MOZ_MUST_USE bool InitProcessExecutableMemory();
extern void ReleaseProcessExecutableMemory();

// Allocate/deallocate executable pages.
extern void* AllocateExecutableMemory(size_t bytes, ProtectionSetting protection);
extern void DeallocateExecutableMemory(void* addr, size_t bytes);

// Returns true if we can allocate a few more MB of executable code without
// hitting our code limit. This function can be used to stop compiling things
// that are optional (like Baseline and Ion code) when we're about to reach the
// limit, so we are less likely to OOM or crash. Note that the limit is
// per-process, so other threads can also allocate code after we call this
// function.
extern bool CanLikelyAllocateMoreExecutableMemory();

} // namespace jit
} // namespace js

#endif // jit_ProcessExecutableMemory_h
