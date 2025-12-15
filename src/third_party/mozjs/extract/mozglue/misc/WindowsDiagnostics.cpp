/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/WindowsDiagnostics.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Types.h"

#include <windows.h>
#include <winternl.h>

#if defined(_M_AMD64)

namespace mozilla {

MOZ_RUNINIT static OnSingleStepCallback sOnSingleStepCallback{};
static void* sOnSingleStepCallbackState = nullptr;
static bool sIsSingleStepping = false;

MFBT_API AutoOnSingleStepCallback::AutoOnSingleStepCallback(
    OnSingleStepCallback aOnSingleStepCallback, void* aState) {
  MOZ_RELEASE_ASSERT(!sIsSingleStepping && !sOnSingleStepCallback &&
                         !sOnSingleStepCallbackState,
                     "Single-stepping is already active");

  sOnSingleStepCallback = std::move(aOnSingleStepCallback);
  sOnSingleStepCallbackState = aState;
  sIsSingleStepping = true;
}

MFBT_API AutoOnSingleStepCallback::~AutoOnSingleStepCallback() {
  sOnSingleStepCallback = OnSingleStepCallback();
  sOnSingleStepCallbackState = nullptr;
  sIsSingleStepping = false;
}

// Going though this assembly code turns on the trap flag, which will trigger
// a first single-step exception. It is then up to the exception handler to
// keep the trap flag enabled so that a new single step exception gets
// triggered with the following instruction.
MFBT_API MOZ_NEVER_INLINE MOZ_NAKED void EnableTrapFlag() {
  asm volatile(
      "pushfq;"
      "orw $0x100,(%rsp);"
      "popfq;"
      "retq;");
}

// This function does not do anything special, but when we reach its address
// while single-stepping the exception handler will know that it is now time to
// leave the trap flag turned off.
MFBT_API MOZ_NEVER_INLINE MOZ_NAKED void DisableTrapFlag() {
  asm volatile("retq;");
}

MFBT_API LONG SingleStepExceptionHandler(_EXCEPTION_POINTERS* aExceptionInfo) {
  if (sIsSingleStepping && sOnSingleStepCallback &&
      aExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    auto instructionPointer = aExceptionInfo->ContextRecord->Rip;
    bool keepOnSingleStepping = false;
    if (instructionPointer != reinterpret_cast<uintptr_t>(&DisableTrapFlag)) {
      keepOnSingleStepping = sOnSingleStepCallback(
          sOnSingleStepCallbackState, aExceptionInfo->ContextRecord);
    }
    if (keepOnSingleStepping) {
      aExceptionInfo->ContextRecord->EFlags |= 0x100;
    } else {
      sIsSingleStepping = false;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace mozilla

#endif  // _M_AMD64
