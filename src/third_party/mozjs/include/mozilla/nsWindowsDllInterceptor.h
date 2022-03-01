/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NS_WINDOWS_DLL_INTERCEPTOR_H_
#define NS_WINDOWS_DLL_INTERCEPTOR_H_

#include <wchar.h>
#include <windows.h>
#include <winternl.h>

#include <utility>

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/NativeNt.h"
#include "mozilla/Tuple.h"
#include "mozilla/Types.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"
#include "mozilla/interceptor/MMPolicies.h"
#include "mozilla/interceptor/PatcherDetour.h"
#include "mozilla/interceptor/PatcherNopSpace.h"
#include "mozilla/interceptor/VMSharingPolicies.h"
#include "nsWindowsHelpers.h"

/*
 * Simple function interception.
 *
 * We have two separate mechanisms for intercepting a function: We can use the
 * built-in nop space, if it exists, or we can create a detour.
 *
 * Using the built-in nop space works as follows: On x86-32, DLL functions
 * begin with a two-byte nop (mov edi, edi) and are preceeded by five bytes of
 * NOP instructions.
 *
 * When we detect a function with this prelude, we do the following:
 *
 * 1. Write a long jump to our interceptor function into the five bytes of NOPs
 *    before the function.
 *
 * 2. Write a short jump -5 into the two-byte nop at the beginning of the
 *    function.
 *
 * This mechanism is nice because it's thread-safe.  It's even safe to do if
 * another thread is currently running the function we're modifying!
 *
 * When the WindowsDllNopSpacePatcher is destroyed, we overwrite the short jump
 * but not the long jump, so re-intercepting the same function won't work,
 * because its prelude won't match.
 *
 *
 * Unfortunately nop space patching doesn't work on functions which don't have
 * this magic prelude (and in particular, x86-64 never has the prelude).  So
 * when we can't use the built-in nop space, we fall back to using a detour,
 * which works as follows:
 *
 * 1. Save first N bytes of OrigFunction to trampoline, where N is a
 *    number of bytes >= 5 that are instruction aligned.
 *
 * 2. Replace first 5 bytes of OrigFunction with a jump to the Hook
 *    function.
 *
 * 3. After N bytes of the trampoline, add a jump to OrigFunction+N to
 *    continue original program flow.
 *
 * 4. Hook function needs to call the trampoline during its execution,
 *    to invoke the original function (so address of trampoline is
 *    returned).
 *
 * When the WindowsDllDetourPatcher object is destructed, OrigFunction is
 * patched again to jump directly to the trampoline instead of going through
 * the hook function. As such, re-intercepting the same function won't work, as
 * jump instructions are not supported.
 *
 * Note that this is not thread-safe.  Sad day.
 *
 */

#if defined(_M_IX86) && defined(__clang__) && __has_declspec_attribute(guard)
// On x86, nop-space patches return to the second instruction of their target.
// This is a deliberate violation of Control Flow Guard, so disable the check.
#  define INTERCEPTOR_DISABLE_CFGUARD __declspec(guard(nocf))
#else
#  define INTERCEPTOR_DISABLE_CFGUARD /* nothing */
#endif

namespace mozilla {
namespace interceptor {

template <typename T>
struct OriginalFunctionPtrTraits;

template <typename R, typename... Args>
struct OriginalFunctionPtrTraits<R (*)(Args...)> {
  using ReturnType = R;
};

#if defined(_M_IX86)
template <typename R, typename... Args>
struct OriginalFunctionPtrTraits<R(__stdcall*)(Args...)> {
  using ReturnType = R;
};

template <typename R, typename... Args>
struct OriginalFunctionPtrTraits<R(__fastcall*)(Args...)> {
  using ReturnType = R;
};
#endif  // defined(_M_IX86)

template <typename InterceptorT, typename FuncPtrT>
class FuncHook final {
 public:
  using ThisType = FuncHook<InterceptorT, FuncPtrT>;
  using ReturnType = typename OriginalFunctionPtrTraits<FuncPtrT>::ReturnType;

  constexpr FuncHook() : mOrigFunc(nullptr), mInitOnce(INIT_ONCE_STATIC_INIT) {}

  ~FuncHook() = default;

  bool Set(InterceptorT& aInterceptor, const char* aName, FuncPtrT aHookDest) {
    LPVOID addHookOk = nullptr;
    InitOnceContext ctx(this, &aInterceptor, aName, aHookDest, false);

    return ::InitOnceExecuteOnce(&mInitOnce, &InitOnceCallback, &ctx,
                                 &addHookOk) &&
           addHookOk;
  }

  bool SetDetour(InterceptorT& aInterceptor, const char* aName,
                 FuncPtrT aHookDest) {
    LPVOID addHookOk = nullptr;
    InitOnceContext ctx(this, &aInterceptor, aName, aHookDest, true);

    return ::InitOnceExecuteOnce(&mInitOnce, &InitOnceCallback, &ctx,
                                 &addHookOk) &&
           addHookOk;
  }

  explicit operator bool() const { return !!mOrigFunc; }

  template <typename... ArgsType>
  INTERCEPTOR_DISABLE_CFGUARD ReturnType operator()(ArgsType&&... aArgs) const {
    return mOrigFunc(std::forward<ArgsType>(aArgs)...);
  }

  FuncPtrT GetStub() const { return mOrigFunc; }

  // One-time init stuff cannot be moved or copied
  FuncHook(const FuncHook&) = delete;
  FuncHook(FuncHook&&) = delete;
  FuncHook& operator=(const FuncHook&) = delete;
  FuncHook& operator=(FuncHook&& aOther) = delete;

 private:
  struct MOZ_RAII InitOnceContext final {
    InitOnceContext(ThisType* aHook, InterceptorT* aInterceptor,
                    const char* aName, FuncPtrT aHookDest, bool aForceDetour)
        : mHook(aHook),
          mInterceptor(aInterceptor),
          mName(aName),
          mHookDest(reinterpret_cast<void*>(aHookDest)),
          mForceDetour(aForceDetour) {}

    ThisType* mHook;
    InterceptorT* mInterceptor;
    const char* mName;
    void* mHookDest;
    bool mForceDetour;
  };

 private:
  bool Apply(InterceptorT* aInterceptor, const char* aName, void* aHookDest) {
    return aInterceptor->AddHook(aName, reinterpret_cast<intptr_t>(aHookDest),
                                 reinterpret_cast<void**>(&mOrigFunc));
  }

  bool ApplyDetour(InterceptorT* aInterceptor, const char* aName,
                   void* aHookDest) {
    return aInterceptor->AddDetour(aName, reinterpret_cast<intptr_t>(aHookDest),
                                   reinterpret_cast<void**>(&mOrigFunc));
  }

  static BOOL CALLBACK InitOnceCallback(PINIT_ONCE aInitOnce, PVOID aParam,
                                        PVOID* aOutContext) {
    MOZ_ASSERT(aOutContext);

    bool result;
    auto ctx = reinterpret_cast<InitOnceContext*>(aParam);
    if (ctx->mForceDetour) {
      result = ctx->mHook->ApplyDetour(ctx->mInterceptor, ctx->mName,
                                       ctx->mHookDest);
    } else {
      result = ctx->mHook->Apply(ctx->mInterceptor, ctx->mName, ctx->mHookDest);
    }

    *aOutContext =
        result ? reinterpret_cast<PVOID>(1U << INIT_ONCE_CTX_RESERVED_BITS)
               : nullptr;
    return TRUE;
  }

 private:
  FuncPtrT mOrigFunc;
  INIT_ONCE mInitOnce;
};

template <typename InterceptorT, typename FuncPtrT>
class MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS FuncHookCrossProcess final {
 public:
  using ThisType = FuncHookCrossProcess<InterceptorT, FuncPtrT>;
  using ReturnType = typename OriginalFunctionPtrTraits<FuncPtrT>::ReturnType;

#if defined(DEBUG)
  FuncHookCrossProcess() {}
#endif  // defined(DEBUG)

  bool Set(nt::CrossExecTransferManager& aTransferMgr,
           InterceptorT& aInterceptor, const char* aName, FuncPtrT aHookDest) {
    FuncPtrT origFunc;
    if (!aInterceptor.AddHook(aName, reinterpret_cast<intptr_t>(aHookDest),
                              reinterpret_cast<void**>(&origFunc))) {
      return false;
    }

    return CopyStubToChildProcess(aTransferMgr, aInterceptor, origFunc);
  }

  bool SetDetour(nt::CrossExecTransferManager& aTransferMgr,
                 InterceptorT& aInterceptor, const char* aName,
                 FuncPtrT aHookDest) {
    FuncPtrT origFunc;
    if (!aInterceptor.AddDetour(aName, reinterpret_cast<intptr_t>(aHookDest),
                                reinterpret_cast<void**>(&origFunc))) {
      return false;
    }

    return CopyStubToChildProcess(aTransferMgr, aInterceptor, origFunc);
  }

  explicit operator bool() const { return !!mOrigFunc; }

  /**
   * NB: This operator is only meaningful when invoked in the target process!
   */
  template <typename... ArgsType>
  ReturnType operator()(ArgsType&&... aArgs) const {
    return mOrigFunc(std::forward<ArgsType>(aArgs)...);
  }

#if defined(DEBUG)
  FuncHookCrossProcess(const FuncHookCrossProcess&) = delete;
  FuncHookCrossProcess(FuncHookCrossProcess&&) = delete;
  FuncHookCrossProcess& operator=(const FuncHookCrossProcess&) = delete;
  FuncHookCrossProcess& operator=(FuncHookCrossProcess&& aOther) = delete;
#endif  // defined(DEBUG)

 private:
  bool CopyStubToChildProcess(nt::CrossExecTransferManager& aTransferMgr,
                              InterceptorT& aInterceptor, FuncPtrT aStub) {
    LauncherVoidResult writeResult =
        aTransferMgr.Transfer(&mOrigFunc, &aStub, sizeof(FuncPtrT));
    if (writeResult.isErr()) {
#ifdef MOZ_USE_LAUNCHER_ERROR
      const mozilla::WindowsError& err = writeResult.inspectErr().mError;
#else
      const mozilla::WindowsError& err = writeResult.inspectErr();
#endif
      aInterceptor.SetLastDetourError(FUNCHOOKCROSSPROCESS_COPYSTUB_ERROR,
                                      err.AsHResult());
      return false;
    }
    return true;
  }

 private:
  FuncPtrT mOrigFunc;
};

template <typename MMPolicyT, typename InterceptorT>
struct TypeResolver;

template <typename InterceptorT>
struct TypeResolver<mozilla::interceptor::MMPolicyInProcess, InterceptorT> {
  template <typename FuncPtrT>
  using FuncHookType = FuncHook<InterceptorT, FuncPtrT>;
};

template <typename InterceptorT>
struct TypeResolver<mozilla::interceptor::MMPolicyOutOfProcess, InterceptorT> {
  template <typename FuncPtrT>
  using FuncHookType = FuncHookCrossProcess<InterceptorT, FuncPtrT>;
};

template <typename VMPolicy = mozilla::interceptor::VMSharingPolicyShared>
class WindowsDllInterceptor final
    : public TypeResolver<typename VMPolicy::MMPolicyT,
                          WindowsDllInterceptor<VMPolicy>> {
  typedef WindowsDllInterceptor<VMPolicy> ThisType;

  interceptor::WindowsDllDetourPatcher<VMPolicy> mDetourPatcher;
#if defined(_M_IX86)
  interceptor::WindowsDllNopSpacePatcher<typename VMPolicy::MMPolicyT>
      mNopSpacePatcher;
#endif  // defined(_M_IX86)

  HMODULE mModule;

 public:
  template <typename... Args>
  explicit WindowsDllInterceptor(Args&&... aArgs)
      : mDetourPatcher(std::forward<Args>(aArgs)...)
#if defined(_M_IX86)
        ,
        mNopSpacePatcher(std::forward<Args>(aArgs)...)
#endif  // defined(_M_IX86)
        ,
        mModule(nullptr) {
  }

  WindowsDllInterceptor(const WindowsDllInterceptor&) = delete;
  WindowsDllInterceptor(WindowsDllInterceptor&&) = delete;
  WindowsDllInterceptor& operator=(const WindowsDllInterceptor&) = delete;
  WindowsDllInterceptor& operator=(WindowsDllInterceptor&&) = delete;

  ~WindowsDllInterceptor() { Clear(); }

  template <size_t N>
  void Init(const char (&aModuleName)[N]) {
    wchar_t moduleName[N];

    for (size_t i = 0; i < N; ++i) {
      MOZ_ASSERT(!(aModuleName[i] & 0x80),
                 "Use wide-character overload for non-ASCII module names");
      moduleName[i] = aModuleName[i];
    }

    Init(moduleName);
  }

  void Init(const wchar_t* aModuleName) {
    if (mModule) {
      return;
    }

    mModule = ::LoadLibraryW(aModuleName);
  }

  /** Force a specific configuration for testing purposes. NOT to be used in
      production code! **/
  void TestOnlyDetourInit(const wchar_t* aModuleName, DetourFlags aFlags) {
    Init(aModuleName);
    mDetourPatcher.Init(aFlags);
  }

  void Clear() {
    if (!mModule) {
      return;
    }

#if defined(_M_IX86)
    mNopSpacePatcher.Clear();
#endif  // defined(_M_IX86)
    mDetourPatcher.Clear();

    // NB: We intentionally leak mModule
  }

#if defined(NIGHTLY_BUILD)
  const Maybe<DetourError>& GetLastDetourError() const {
    return mDetourPatcher.GetLastDetourError();
  }
#endif  // defined(NIGHTLY_BUILD)
  template <typename... Args>
  void SetLastDetourError(Args&&... aArgs) {
    return mDetourPatcher.SetLastDetourError(std::forward<Args>(aArgs)...);
  }

  constexpr static uint32_t GetWorstCaseRequiredBytesToPatch() {
    return WindowsDllDetourPatcherPrimitive<
        typename VMPolicy::MMPolicyT>::GetWorstCaseRequiredBytesToPatch();
  }

 private:
  /**
   * Hook/detour the method aName from the DLL we set in Init so that it calls
   * aHookDest instead.  Returns the original method pointer in aOrigFunc
   * and returns true if successful.
   *
   * IMPORTANT: If you use this method, please add your case to the
   * TestDllInterceptor in order to detect future failures.  Even if this
   * succeeds now, updates to the hooked DLL could cause it to fail in
   * the future.
   */
  bool AddHook(const char* aName, intptr_t aHookDest, void** aOrigFunc) {
    // Use a nop space patch if possible, otherwise fall back to a detour.
    // This should be the preferred method for adding hooks.
    if (!mModule) {
      mDetourPatcher.SetLastDetourError(DetourResultCode::INTERCEPTOR_MOD_NULL);
      return false;
    }

    if (!mDetourPatcher.IsPageAccessible(
            nt::PEHeaders::HModuleToBaseAddr<uintptr_t>(mModule))) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_MOD_INACCESSIBLE);
      return false;
    }

    FARPROC proc = mDetourPatcher.GetProcAddress(mModule, aName);
    if (!proc) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_PROC_NULL);
      return false;
    }

    if (!mDetourPatcher.IsPageAccessible(reinterpret_cast<uintptr_t>(proc))) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_PROC_INACCESSIBLE);
      return false;
    }

#if defined(_M_IX86)
    if (mNopSpacePatcher.AddHook(proc, aHookDest, aOrigFunc)) {
      return true;
    }
#endif  // defined(_M_IX86)

    return AddDetour(proc, aHookDest, aOrigFunc);
  }

  /**
   * Detour the method aName from the DLL we set in Init so that it calls
   * aHookDest instead.  Returns the original method pointer in aOrigFunc
   * and returns true if successful.
   *
   * IMPORTANT: If you use this method, please add your case to the
   * TestDllInterceptor in order to detect future failures.  Even if this
   * succeeds now, updates to the detoured DLL could cause it to fail in
   * the future.
   */
  bool AddDetour(const char* aName, intptr_t aHookDest, void** aOrigFunc) {
    // Generally, code should not call this method directly. Use AddHook unless
    // there is a specific need to avoid nop space patches.
    if (!mModule) {
      mDetourPatcher.SetLastDetourError(DetourResultCode::INTERCEPTOR_MOD_NULL);
      return false;
    }

    if (!mDetourPatcher.IsPageAccessible(
            nt::PEHeaders::HModuleToBaseAddr<uintptr_t>(mModule))) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_MOD_INACCESSIBLE);
      return false;
    }

    FARPROC proc = mDetourPatcher.GetProcAddress(mModule, aName);
    if (!proc) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_PROC_NULL);
      return false;
    }

    if (!mDetourPatcher.IsPageAccessible(reinterpret_cast<uintptr_t>(proc))) {
      mDetourPatcher.SetLastDetourError(
          DetourResultCode::INTERCEPTOR_PROC_INACCESSIBLE);
      return false;
    }

    return AddDetour(proc, aHookDest, aOrigFunc);
  }

  bool AddDetour(FARPROC aProc, intptr_t aHookDest, void** aOrigFunc) {
    MOZ_ASSERT(mModule && aProc);

    if (!mDetourPatcher.Initialized()) {
      DetourFlags flags = DetourFlags::eDefault;
#if defined(_M_X64)
      // NTDLL hooks should attempt to use a 10-byte patch because some
      // injected DLLs do the same and interfere with our stuff.
      bool needs10BytePatch = (mModule == ::GetModuleHandleW(L"ntdll.dll"));

      bool isWin8Or81 = IsWin8OrLater() && (!IsWin10OrLater());
      bool isWin8 = IsWin8OrLater() && (!IsWin8Point1OrLater());

      bool isKernel32Dll = (mModule == ::GetModuleHandleW(L"kernel32.dll"));

      bool isDuplicateHandle = (reinterpret_cast<void*>(aProc) ==
                                reinterpret_cast<void*>(&::DuplicateHandle));

      // CloseHandle on Windows 8/8.1 only accomodates 10-byte patches.
      needs10BytePatch |= isWin8Or81 && isKernel32Dll &&
                          (reinterpret_cast<void*>(aProc) ==
                           reinterpret_cast<void*>(&CloseHandle));

      // CreateFileA and DuplicateHandle on Windows 8 require 10-byte patches.
      needs10BytePatch |= isWin8 && isKernel32Dll &&
                          ((reinterpret_cast<void*>(aProc) ==
                            reinterpret_cast<void*>(&::CreateFileA)) ||
                           isDuplicateHandle);

      if (needs10BytePatch) {
        flags |= DetourFlags::eEnable10BytePatch;
      }

      if (isWin8 && isDuplicateHandle) {
        // Because we can't detour Win8's KERNELBASE!DuplicateHandle,
        // we detour kernel32!DuplicateHandle (See bug 1659398).
        flags |= DetourFlags::eDontResolveRedirection;
      }
#endif  // defined(_M_X64)

      mDetourPatcher.Init(flags);
    }

    return mDetourPatcher.AddHook(aProc, aHookDest, aOrigFunc);
  }

 private:
  template <typename InterceptorT, typename FuncPtrT>
  friend class FuncHook;

  template <typename InterceptorT, typename FuncPtrT>
  friend class FuncHookCrossProcess;
};

/**
 * IAT patching is intended for use when we only want to intercept a function
 * call originating from a specific module.
 */
class WindowsIATPatcher final {
 public:
  template <typename FuncPtrT>
  using FuncHookType = FuncHook<WindowsIATPatcher, FuncPtrT>;

 private:
  static bool CheckASCII(const char* aInStr) {
    while (*aInStr) {
      if (*aInStr & 0x80) {
        return false;
      }
      ++aInStr;
    }
    return true;
  }

  static bool AddHook(HMODULE aFromModule, const char* aToModuleName,
                      const char* aTargetFnName, void* aHookDest,
                      Atomic<void*>* aOutOrigFunc) {
    if (!aFromModule || !aToModuleName || !aTargetFnName || !aOutOrigFunc) {
      return false;
    }

    // PE Spec requires ASCII names for imported module names
    const bool isModuleNameAscii = CheckASCII(aToModuleName);
    MOZ_ASSERT(isModuleNameAscii);
    if (!isModuleNameAscii) {
      return false;
    }

    // PE Spec requires ASCII names for imported function names
    const bool isTargetFnNameAscii = CheckASCII(aTargetFnName);
    MOZ_ASSERT(isTargetFnNameAscii);
    if (!isTargetFnNameAscii) {
      return false;
    }

    nt::PEHeaders headers(aFromModule);
    if (!headers) {
      return false;
    }

    PIMAGE_IMPORT_DESCRIPTOR impDesc =
        headers.GetImportDescriptor(aToModuleName);
    if (!nt::PEHeaders::IsValid(impDesc)) {
      // Either aFromModule does not import aToModuleName at load-time, or
      // aToModuleName is a (currently unsupported) delay-load import.
      return false;
    }

    // Resolve the import name table (INT).
    auto firstINTThunk = headers.template RVAToPtr<PIMAGE_THUNK_DATA>(
        impDesc->OriginalFirstThunk);
    if (!nt::PEHeaders::IsValid(firstINTThunk)) {
      return false;
    }

    Maybe<ptrdiff_t> thunkIndex;

    // Scan the INT for the location of the thunk for the function named
    // 'aTargetFnName'.
    for (PIMAGE_THUNK_DATA curINTThunk = firstINTThunk;
         nt::PEHeaders::IsValid(curINTThunk); ++curINTThunk) {
      if (IMAGE_SNAP_BY_ORDINAL(curINTThunk->u1.Ordinal)) {
        // Currently not supporting import by ordinal; this isn't hard to add,
        // but we won't bother unless necessary.
        continue;
      }

      PIMAGE_IMPORT_BY_NAME curThunkFnName =
          headers.template RVAToPtr<PIMAGE_IMPORT_BY_NAME>(
              curINTThunk->u1.AddressOfData);
      MOZ_ASSERT(curThunkFnName);
      if (!curThunkFnName) {
        // Looks like we have a bad name descriptor. Try to continue.
        continue;
      }

      // Function name checks MUST be case-sensitive!
      if (!strcmp(aTargetFnName, curThunkFnName->Name)) {
        // We found the thunk. Save the index of this thunk, as the IAT thunk
        // is located at the same index in that table as in the INT.
        thunkIndex = Some(curINTThunk - firstINTThunk);
        break;
      }
    }

    if (thunkIndex.isNothing()) {
      // We never found a thunk for that function. Perhaps it's not imported?
      return false;
    }

    if (thunkIndex.value() < 0) {
      // That's just wrong.
      return false;
    }

    auto firstIATThunk =
        headers.template RVAToPtr<PIMAGE_THUNK_DATA>(impDesc->FirstThunk);
    if (!nt::PEHeaders::IsValid(firstIATThunk)) {
      return false;
    }

    // Resolve the IAT thunk for the function we want
    PIMAGE_THUNK_DATA targetThunk = &firstIATThunk[thunkIndex.value()];
    if (!nt::PEHeaders::IsValid(targetThunk)) {
      return false;
    }

    auto fnPtr = reinterpret_cast<Atomic<void*>*>(&targetThunk->u1.Function);

    // Now we can just change out its pointer with our hook function.
    AutoVirtualProtect prot(fnPtr, sizeof(void*), PAGE_EXECUTE_READWRITE);
    if (!prot) {
      return false;
    }

    // We do the exchange this way to ensure that *aOutOrigFunc is always valid
    // once the atomic exchange has taken place.
    void* tmp;

    do {
      tmp = *fnPtr;
      *aOutOrigFunc = tmp;
    } while (!fnPtr->compareExchange(tmp, aHookDest));

    return true;
  }

  template <typename InterceptorT, typename FuncPtrT>
  friend class FuncHook;
};

template <typename FuncPtrT>
class MOZ_ONLY_USED_TO_AVOID_STATIC_CONSTRUCTORS
    FuncHook<WindowsIATPatcher, FuncPtrT>
        final {
 public:
  using ThisType = FuncHook<WindowsIATPatcher, FuncPtrT>;
  using ReturnType = typename OriginalFunctionPtrTraits<FuncPtrT>::ReturnType;

  constexpr FuncHook()
      : mInitOnce(INIT_ONCE_STATIC_INIT),
        mFromModule(nullptr),
        mOrigFunc(nullptr) {}

#if defined(DEBUG)
  ~FuncHook() = default;
#endif  // defined(DEBUG)

  bool Set(const wchar_t* aFromModuleName, const char* aToModuleName,
           const char* aFnName, FuncPtrT aHookDest) {
    nsModuleHandle fromModule(::LoadLibraryW(aFromModuleName));
    if (!fromModule) {
      return false;
    }

    return Set(fromModule, aToModuleName, aFnName, aHookDest);
  }

  // We offer this overload in case the client wants finer-grained control over
  // loading aFromModule.
  bool Set(nsModuleHandle& aFromModule, const char* aToModuleName,
           const char* aFnName, FuncPtrT aHookDest) {
    LPVOID addHookOk = nullptr;
    InitOnceContext ctx(this, aFromModule, aToModuleName, aFnName, aHookDest);

    bool result = ::InitOnceExecuteOnce(&mInitOnce, &InitOnceCallback, &ctx,
                                        &addHookOk) &&
                  addHookOk;
    if (!result) {
      return result;
    }

    // If we successfully set the hook then we must retain a strong reference
    // to the module that we modified.
    mFromModule = aFromModule.disown();
    return result;
  }

  explicit operator bool() const { return !!mOrigFunc; }

  template <typename... ArgsType>
  ReturnType operator()(ArgsType&&... aArgs) const {
    return mOrigFunc(std::forward<ArgsType>(aArgs)...);
  }

  FuncPtrT GetStub() const { return mOrigFunc; }

#if defined(DEBUG)
  // One-time init stuff cannot be moved or copied
  FuncHook(const FuncHook&) = delete;
  FuncHook(FuncHook&&) = delete;
  FuncHook& operator=(const FuncHook&) = delete;
  FuncHook& operator=(FuncHook&& aOther) = delete;
#endif  // defined(DEBUG)

 private:
  struct MOZ_RAII InitOnceContext final {
    InitOnceContext(ThisType* aHook, const nsModuleHandle& aFromModule,
                    const char* aToModuleName, const char* aFnName,
                    FuncPtrT aHookDest)
        : mHook(aHook),
          mFromModule(aFromModule),
          mToModuleName(aToModuleName),
          mFnName(aFnName),
          mHookDest(reinterpret_cast<void*>(aHookDest)) {}

    ThisType* mHook;
    const nsModuleHandle& mFromModule;
    const char* mToModuleName;
    const char* mFnName;
    void* mHookDest;
  };

 private:
  bool Apply(const nsModuleHandle& aFromModule, const char* aToModuleName,
             const char* aFnName, void* aHookDest) {
    return WindowsIATPatcher::AddHook(
        aFromModule, aToModuleName, aFnName, aHookDest,
        reinterpret_cast<Atomic<void*>*>(&mOrigFunc));
  }

  static BOOL CALLBACK InitOnceCallback(PINIT_ONCE aInitOnce, PVOID aParam,
                                        PVOID* aOutContext) {
    MOZ_ASSERT(aOutContext);

    auto ctx = reinterpret_cast<InitOnceContext*>(aParam);
    bool result = ctx->mHook->Apply(ctx->mFromModule, ctx->mToModuleName,
                                    ctx->mFnName, ctx->mHookDest);

    *aOutContext =
        result ? reinterpret_cast<PVOID>(1U << INIT_ONCE_CTX_RESERVED_BITS)
               : nullptr;
    return TRUE;
  }

 private:
  INIT_ONCE mInitOnce;
  HMODULE mFromModule;  // never freed
  FuncPtrT mOrigFunc;
};

/**
 * This class applies an irreversible patch to jump to a target function
 * without backing up the original function.
 */
class WindowsDllEntryPointInterceptor final {
  using DllMainFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
  using MMPolicyT = MMPolicyInProcessEarlyStage;

  MMPolicyT mMMPolicy;

 public:
  explicit WindowsDllEntryPointInterceptor(
      const MMPolicyT::Kernel32Exports& aK32Exports)
      : mMMPolicy(aK32Exports) {}

  bool Set(const nt::PEHeaders& aHeaders, DllMainFn aDestination) {
    if (!aHeaders) {
      return false;
    }

    WindowsDllDetourPatcherPrimitive<MMPolicyT> patcher;
    return patcher.AddIrreversibleHook(
        mMMPolicy, aHeaders.GetEntryPoint(),
        reinterpret_cast<uintptr_t>(aDestination));
  }
};

}  // namespace interceptor

using WindowsDllInterceptor = interceptor::WindowsDllInterceptor<>;

using CrossProcessDllInterceptor = interceptor::WindowsDllInterceptor<
    mozilla::interceptor::VMSharingPolicyUnique<
        mozilla::interceptor::MMPolicyOutOfProcess>>;

using WindowsIATPatcher = interceptor::WindowsIATPatcher;

}  // namespace mozilla

#endif /* NS_WINDOWS_DLL_INTERCEPTOR_H_ */
