/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WindowsDiagnostics_h
#define mozilla_WindowsDiagnostics_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Types.h"

// Bug 1898761: NativeNt.h depends on headers that live outside mozglue/misc/
// and are not available in SpiderMonkey builds. Until we fix this, we cannot
// depend on NativeNt.h in mozglue/misc/ cpp files.
#if !defined(IMPL_MFBT)
#  include "mozilla/NativeNt.h"
#endif  // !IMPL_MFBT

#include <windows.h>
#include <winternl.h>

#include <functional>

namespace mozilla {

enum class WindowsDiagnosticsError : uint32_t {
  None,
  InternalFailure,
  DebuggerPresent,
  ModuleNotFound,
  BadModule,
};

// Struct storing the visible, loggable error-state of a Windows thread.
// Approximately `std::pair(::GetLastError(), ::RtlGetLastNtStatus())`.
//
// Uses sentinel values rather than a proper `Maybe` type to simplify
// minidump-analysis.
struct WinErrorState {
  // Last error, as provided by ::GetLastError().
  DWORD error = ~0;
  // Last NTSTATUS, as provided by the TIB.
  NTSTATUS ntStatus = ~0;

 private:
  // per WINE et al.; stable since NT 3.51
  constexpr static size_t kLastNtStatusOffset =
      sizeof(size_t) == 8 ? 0x1250 : 0xbf4;

  static void SetLastNtStatus(NTSTATUS status) {
    auto* teb = ::NtCurrentTeb();
    *reinterpret_cast<NTSTATUS*>(reinterpret_cast<char*>(teb) +
                                 kLastNtStatusOffset) = status;
  }

  static NTSTATUS GetLastNtStatus() {
    auto const* teb = ::NtCurrentTeb();
    return *reinterpret_cast<NTSTATUS const*>(
        reinterpret_cast<char const*>(teb) + kLastNtStatusOffset);
  }

 public:
  // Restore (or just set) the error state of the current thread.
  static void Apply(WinErrorState const& state) {
    SetLastNtStatus(state.ntStatus);
    ::SetLastError(state.error);
  }

  // Clear the error-state of the current thread.
  static void Clear() { Apply({.error = 0, .ntStatus = 0}); }

  // Get the error-state of the current thread.
  static WinErrorState Get() {
    return WinErrorState{
        .error = ::GetLastError(),
        .ntStatus = GetLastNtStatus(),
    };
  }

  bool operator==(WinErrorState const& that) const {
    return this->error == that.error && this->ntStatus == that.ntStatus;
  }

  bool operator!=(WinErrorState const& that) const { return !operator==(that); }
};

// TODO This code does not have tests. Only use it on paths that are already
//      known to crash. Add tests before using it in release builds.
#if defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED) && defined(_M_X64)

using OnSingleStepCallback = std::function<bool(void*, CONTEXT*)>;

class MOZ_RAII AutoOnSingleStepCallback {
 public:
  MFBT_API AutoOnSingleStepCallback(OnSingleStepCallback aOnSingleStepCallback,
                                    void* aState);
  MFBT_API ~AutoOnSingleStepCallback();

  AutoOnSingleStepCallback(const AutoOnSingleStepCallback&) = delete;
  AutoOnSingleStepCallback(AutoOnSingleStepCallback&&) = delete;
  AutoOnSingleStepCallback& operator=(const AutoOnSingleStepCallback&) = delete;
  AutoOnSingleStepCallback& operator=(AutoOnSingleStepCallback&&) = delete;
};

MFBT_API MOZ_NEVER_INLINE __attribute__((naked)) void EnableTrapFlag();
MFBT_API MOZ_NEVER_INLINE __attribute__((naked)) void DisableTrapFlag();
MFBT_API LONG SingleStepExceptionHandler(_EXCEPTION_POINTERS* aExceptionInfo);

// This block uses nt::PEHeaders and thus depends on NativeNt.h.
#  if !defined(IMPL_MFBT)

// Run aCallbackToRun instruction by instruction, and between each instruction
// call aOnSingleStepCallback. Single-stepping ends when aOnSingleStepCallback
// returns false (in which case aCallbackToRun will continue to run
// unmonitored), or when we reach the end of aCallbackToRun.
template <typename CallbackToRun>
[[clang::optnone]] MOZ_NEVER_INLINE WindowsDiagnosticsError
CollectSingleStepData(CallbackToRun aCallbackToRun,
                      OnSingleStepCallback aOnSingleStepCallback,
                      void* aOnSingleStepCallbackState) {
  if (::IsDebuggerPresent()) {
    return WindowsDiagnosticsError::DebuggerPresent;
  }

  AutoOnSingleStepCallback setCallback(std::move(aOnSingleStepCallback),
                                       aOnSingleStepCallbackState);

  auto veh = ::AddVectoredExceptionHandler(TRUE, SingleStepExceptionHandler);
  if (!veh) {
    return WindowsDiagnosticsError::InternalFailure;
  }

  EnableTrapFlag();
  aCallbackToRun();
  DisableTrapFlag();
  ::RemoveVectoredExceptionHandler(veh);

  return WindowsDiagnosticsError::None;
}

template <int NMaxSteps, int NMaxErrorStates>
struct ModuleSingleStepData {
  uint32_t mStepsLog[NMaxSteps]{};
  WinErrorState mErrorStatesLog[NMaxErrorStates]{};
  uint16_t mStepsAtErrorState[NMaxErrorStates]{};
};

template <int NMaxSteps, int NMaxErrorStates>
struct ModuleSingleStepState {
  uintptr_t mModuleStart;
  uintptr_t mModuleEnd;
  uint32_t mSteps;
  uint32_t mErrorStates;
  WinErrorState mLastRecordedErrorState;
  ModuleSingleStepData<NMaxSteps, NMaxErrorStates> mData;

  ModuleSingleStepState(uintptr_t aModuleStart, uintptr_t aModuleEnd)
      : mModuleStart{aModuleStart},
        mModuleEnd{aModuleEnd},
        mSteps{},
        mErrorStates{},
        mLastRecordedErrorState{},
        mData{} {}
};

namespace InstructionFilter {

// These functions return true if the instruction behind aInstructionPointer
// should be recorded during single-stepping.

inline bool All(const uint8_t* aInstructionPointer) { return true; }

// Note: This filter does *not* currently identify all call/ret instructions.
//       For example, prefixed instructions are not recognized.
inline bool CallRet(const uint8_t* aInstructionPointer) {
  auto firstByte = aInstructionPointer[0];
  // E8: CALL rel. Call near, relative.
  if (firstByte == 0xe8) {
    return true;
  }
  // FF /2: CALL r.	Call near, absolute indirect.
  else if (firstByte == 0xff) {
    auto secondByte = aInstructionPointer[1];
    if ((secondByte & 0x38) == 0x10) {
      return true;
    }
  }
  // C3: RET. Near return.
  else if (firstByte == 0xc3) {
    return true;
  }
  // C2: RET imm. Near return and pop imm bytes.
  else if (firstByte == 0xc2) {
    return true;
  }
  return false;
}

}  // namespace InstructionFilter

// This function runs aCallbackToRun instruction by instruction, recording
// information about the paths taken within a specific module given by
// aModulePath. It then calls aPostCollectionCallback with the collected data.
//
// We store the collected data in stack, so that it is available in crash
// reports in case we decide to crash from aPostCollectionCallback. Remember to
// carefully estimate the stack usage when choosing NMaxSteps and
// NMaxErrorStates. Consider using an InstructionFilter if you need to reduce
// the number of steps that get recorded.
//
// This function is typically useful on known-to-crash paths, where we can
// replace the crash by a new single-stepped attempt at doing the operation
// that just failed. If the operation fails while single-stepped, we'll be able
// to produce a crash report that contains single step data, which may prove
// useful to understand why the operation failed.
template <
    int NMaxSteps, int NMaxErrorStates, typename CallbackToRun,
    typename PostCollectionCallback,
    typename InstructionFilterCallback = decltype(&InstructionFilter::All)>
WindowsDiagnosticsError CollectModuleSingleStepData(
    const wchar_t* aModulePath, CallbackToRun aCallbackToRun,
    PostCollectionCallback aPostCollectionCallback,
    InstructionFilterCallback aInstructionFilter = InstructionFilter::All) {
  HANDLE mod = ::GetModuleHandleW(aModulePath);
  if (!mod) {
    return WindowsDiagnosticsError::ModuleNotFound;
  }

  nt::PEHeaders headers{mod};
  auto maybeBounds = headers.GetBounds();
  if (maybeBounds.isNothing()) {
    return WindowsDiagnosticsError::BadModule;
  }

  auto& bounds = maybeBounds.ref();
  using State = ModuleSingleStepState<NMaxSteps, NMaxErrorStates>;
  State state{reinterpret_cast<uintptr_t>(bounds.begin().get()),
              reinterpret_cast<uintptr_t>(bounds.end().get())};

  WindowsDiagnosticsError rv = CollectSingleStepData(
      std::move(aCallbackToRun),
      [&aInstructionFilter](void* aState, CONTEXT* aContextRecord) -> bool {
        auto& state = *reinterpret_cast<State*>(aState);
        auto instructionPointer = aContextRecord->Rip;
        // Record data for the current step, if in module
        if (state.mModuleStart <= instructionPointer &&
            instructionPointer < state.mModuleEnd &&
            aInstructionFilter(
                reinterpret_cast<const uint8_t*>(instructionPointer))) {
          // We record the instruction pointer
          if (state.mSteps < NMaxSteps) {
            state.mData.mStepsLog[state.mSteps] =
                static_cast<uint32_t>(instructionPointer - state.mModuleStart);
          }

          // We record changes in the error state
          auto currentErrorState{WinErrorState::Get()};
          if (currentErrorState != state.mLastRecordedErrorState) {
            state.mLastRecordedErrorState = currentErrorState;

            if (state.mErrorStates < NMaxErrorStates) {
              state.mData.mErrorStatesLog[state.mErrorStates] =
                  currentErrorState;
              state.mData.mStepsAtErrorState[state.mErrorStates] = state.mSteps;
            }

            ++state.mErrorStates;
          }

          ++state.mSteps;
        }

        // Continue single-stepping
        return true;
      },
      reinterpret_cast<void*>(&state));

  if (rv != WindowsDiagnosticsError::None) {
    return rv;
  }

  aPostCollectionCallback(state.mData);

  return WindowsDiagnosticsError::None;
}

#  endif  // !IMPL_MFBT

#endif  // MOZ_DIAGNOSTIC_ASSERT_ENABLED && _M_X64

}  // namespace mozilla

#endif  // mozilla_WindowsDiagnostics_h
