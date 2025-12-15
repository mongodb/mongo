/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* API for getting a stack trace of the C/C++ stack on the current thread */

#include "mozilla/Array.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/StackWalk.h"
#ifdef XP_WIN
#  include "mozilla/StackWalkThread.h"
#  include <io.h>
#else
#  include <unistd.h>
#endif
#include "mozilla/Sprintf.h"

#include <string.h>

#if defined(ANDROID) && defined(MOZ_LINKER)
#  include "Linker.h"
#  include <android/log.h>
#endif

using namespace mozilla;

// for _Unwind_Backtrace from libcxxrt or libunwind
// cxxabi.h from libcxxrt implicitly includes unwind.h first
#if defined(HAVE__UNWIND_BACKTRACE) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#if defined(HAVE_DLFCN_H) || defined(XP_DARWIN)
#  include <dlfcn.h>
#endif

#if (defined(XP_DARWIN) && \
     (defined(__i386) || defined(__ppc__) || defined(HAVE__UNWIND_BACKTRACE)))
#  define MOZ_STACKWALK_SUPPORTS_MACOSX 1
#else
#  define MOZ_STACKWALK_SUPPORTS_MACOSX 0
#endif

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 1)
#  define HAVE___LIBC_STACK_END 1
#else
#  define HAVE___LIBC_STACK_END 0
#endif

#if (defined(linux) &&                                            \
     ((defined(__GNUC__) && (defined(__i386) || defined(PPC))) || \
      defined(HAVE__UNWIND_BACKTRACE)) &&                         \
     (HAVE___LIBC_STACK_END || ANDROID))
#  define MOZ_STACKWALK_SUPPORTS_LINUX 1
#else
#  define MOZ_STACKWALK_SUPPORTS_LINUX 0
#endif

#if HAVE___LIBC_STACK_END
extern MOZ_EXPORT void* __libc_stack_end;  // from ld-linux.so
#  ifdef __aarch64__
static Atomic<uintptr_t> ldso_base;
#  endif
#endif

#ifdef ANDROID
#  include <algorithm>
#  include <unistd.h>
#  include <pthread.h>
#endif

class FrameSkipper {
 public:
  constexpr FrameSkipper() : mSkipUntilAddr(0) {}
  static uintptr_t AddressFromPC(const void* aPC) {
#ifdef __arm__
    // On 32-bit ARM, mask off the thumb bit to get the instruction address.
    return uintptr_t(aPC) & ~1;
#else
    return uintptr_t(aPC);
#endif
  }
  bool ShouldSkipPC(void* aPC) {
    // Skip frames until we encounter the one we were initialized with,
    // and then never skip again.
    uintptr_t instructionAddress = AddressFromPC(aPC);
    if (mSkipUntilAddr != 0) {
      if (mSkipUntilAddr != instructionAddress) {
        return true;
      }
      mSkipUntilAddr = 0;
    }
    return false;
  }
  explicit FrameSkipper(const void* aPC) : mSkipUntilAddr(AddressFromPC(aPC)) {}

 private:
  uintptr_t mSkipUntilAddr;
};

#ifdef XP_WIN

#  include <windows.h>
#  include <process.h>
#  include <stdio.h>
#  include <malloc.h>
#  include "mozilla/ArrayUtils.h"
#  include "mozilla/Atomics.h"
#  include "mozilla/StackWalk_windows.h"
#  include "mozilla/WindowsVersion.h"

#  include <imagehlp.h>
// We need a way to know if we are building for WXP (or later), as if we are, we
// need to use the newer 64-bit APIs. API_VERSION_NUMBER seems to fit the bill.
// A value of 9 indicates we want to use the new APIs.
#  if API_VERSION_NUMBER < 9
#    error Too old imagehlp.h
#  endif

#  if defined(_M_AMD64) || defined(_M_ARM64)
// We must use RtlLookupFunctionEntry to do stack walking on x86-64 and arm64,
// but internally this function does a blocking shared acquire of SRW locks
// that live in ntdll and are not exported. This is problematic when we want to
// suspend a thread and walk its stack, like we do in the profiler and the
// background hang reporter. If the suspended thread happens to hold one of the
// locks exclusively while suspended, then the stack walking thread will
// deadlock if it calls RtlLookupFunctionEntry.
//
// Note that we only care about deadlocks between the stack walking thread and
// the suspended thread. Any other deadlock scenario is considered out of
// scope, because they are unlikely to be our fault -- these other scenarios
// imply that some thread that we did not suspend is stuck holding one of the
// locks exclusively, and exclusive acquisition of these locks only happens for
// a brief time during Microsoft API calls (e.g. LdrLoadDll, LdrUnloadDll).
//
// We use one of two alternative strategies to gracefully fail to capture a
// stack instead of running into a deadlock:
//    (1) collect pointers to the ntdll internal locks at stack walk
//        initialization, then try to acquire them non-blockingly before
//        initiating any stack walk;
// or (2) mark all code paths that can potentially end up doing an exclusive
//        acquisition of the locks as stack walk suppression paths, then check
//        if any thread is currently on a stack walk suppression path before
//        initiating any stack walk;
//
// Strategy (2) can only avoid all deadlocks under the easily wronged
// assumption that we have correctly identified all existing paths that should
// be stack suppression paths. With strategy (2) we cannot collect stacks e.g.
// during the whole duration of a DLL load happening on any thread so the
// profiling results are worse.
//
// Strategy (1) guarantees no deadlock. It also gives better profiling results
// because it is more fine-grained. Therefore we always prefer strategy (1),
// and we only use strategy (2) as a fallback.

// Strategy (1): Ntdll Internal Locks
//
// The external stack walk initialization code will feed us pointers to the
// ntdll internal locks. Once we have them, we no longer need to rely on
// strategy (2).
static Atomic<bool> sStackWalkLocksInitialized;
static Array<SRWLOCK*, 2> sStackWalkLocks;

MFBT_API
void InitializeStackWalkLocks(const Array<void*, 2>& aStackWalkLocks) {
  sStackWalkLocks[0] = reinterpret_cast<SRWLOCK*>(aStackWalkLocks[0]);
  sStackWalkLocks[1] = reinterpret_cast<SRWLOCK*>(aStackWalkLocks[1]);
  sStackWalkLocksInitialized = true;
}

// Strategy (2): Stack Walk Suppressions
//
// We're using an atomic counter rather than a critical section because we
// don't require mutual exclusion with the stack walker. If the stack walker
// determines that it's safe to start unwinding the suspended thread (i.e.
// there are no suppressions when the unwind begins), then it's safe to
// continue unwinding that thread even if other threads request suppressions
// in the meantime, because we can't deadlock with those other threads.
//
// XXX: This global variable is a larger-than-necessary hammer. A more scoped
// solution would be to maintain a counter per thread, but then it would be
// more difficult for WalkStackMain64 to read the suspended thread's counter.
static Atomic<size_t> sStackWalkSuppressions;

void SuppressStackWalking() { ++sStackWalkSuppressions; }

void DesuppressStackWalking() {
  auto previousValue = sStackWalkSuppressions--;
  // We should never desuppress from 0. See bug 1687510 comment 10 for an
  // example in which this occured.
  MOZ_RELEASE_ASSERT(previousValue);
}

MFBT_API
AutoSuppressStackWalking::AutoSuppressStackWalking() { SuppressStackWalking(); }

MFBT_API
AutoSuppressStackWalking::~AutoSuppressStackWalking() {
  DesuppressStackWalking();
}

bool IsStackWalkingSafe() {
  // Use strategy (1), if initialized.
  if (sStackWalkLocksInitialized) {
    bool isSafe = false;
    if (::TryAcquireSRWLockShared(sStackWalkLocks[0])) {
      if (::TryAcquireSRWLockShared(sStackWalkLocks[1])) {
        isSafe = true;
        ::ReleaseSRWLockShared(sStackWalkLocks[1]);
      }
      ::ReleaseSRWLockShared(sStackWalkLocks[0]);
    }
    return isSafe;
  }

  // Otherwise, fall back to strategy (2).
  return sStackWalkSuppressions == 0;
}

static uint8_t* sJitCodeRegionStart;
static size_t sJitCodeRegionSize;
uint8_t* sMsMpegJitCodeRegionStart;
size_t sMsMpegJitCodeRegionSize;

MFBT_API void RegisterJitCodeRegion(uint8_t* aStart, size_t aSize) {
  // Currently we can only handle one JIT code region at a time
  MOZ_RELEASE_ASSERT(!sJitCodeRegionStart);

  sJitCodeRegionStart = aStart;
  sJitCodeRegionSize = aSize;
}

MFBT_API void UnregisterJitCodeRegion(uint8_t* aStart, size_t aSize) {
  // Currently we can only handle one JIT code region at a time
  MOZ_RELEASE_ASSERT(sJitCodeRegionStart && sJitCodeRegionStart == aStart &&
                     sJitCodeRegionSize == aSize);

  sJitCodeRegionStart = nullptr;
  sJitCodeRegionSize = 0;
}

#  endif  // _M_AMD64 || _M_ARM64

// Routine to print an error message to standard error.
static void PrintError(const char* aPrefix) {
  LPSTR lpMsgBuf;
  DWORD lastErr = GetLastError();
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, lastErr,
                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),  // Default language
                 (LPSTR)&lpMsgBuf, 0, nullptr);
  fprintf(stderr, "### ERROR: %s: %s", aPrefix,
          lpMsgBuf ? lpMsgBuf : "(null)\n");
  fflush(stderr);
  LocalFree(lpMsgBuf);
}

class MOZ_RAII AutoCriticalSection {
 public:
  explicit inline AutoCriticalSection(LPCRITICAL_SECTION aCriticalSection)
      : mCriticalSection{aCriticalSection} {
    ::EnterCriticalSection(mCriticalSection);
  }
  inline ~AutoCriticalSection() { ::LeaveCriticalSection(mCriticalSection); }

  AutoCriticalSection(AutoCriticalSection&& other) = delete;
  AutoCriticalSection operator=(AutoCriticalSection&& other) = delete;
  AutoCriticalSection(const AutoCriticalSection&) = delete;
  AutoCriticalSection operator=(const AutoCriticalSection&) = delete;

 private:
  LPCRITICAL_SECTION mCriticalSection;
};

// A thread-safe safe object interface for Microsoft's DbgHelp.dll. DbgHelp
// APIs are not thread-safe and they require the use of a unique HANDLE value
// as an identifier for the current session. All this is handled internally by
// DbgHelpWrapper.
class DbgHelpWrapper {
 public:
  explicit inline DbgHelpWrapper() : DbgHelpWrapper(InitFlag::BasicInit) {}
  DbgHelpWrapper(DbgHelpWrapper&& other) = delete;
  DbgHelpWrapper operator=(DbgHelpWrapper&& other) = delete;
  DbgHelpWrapper(const DbgHelpWrapper&) = delete;
  DbgHelpWrapper operator=(const DbgHelpWrapper&) = delete;

  inline bool ReadyToUse() { return mInitSuccess; }

  inline BOOL StackWalk64(
      DWORD aMachineType, HANDLE aThread, LPSTACKFRAME64 aStackFrame,
      PVOID aContextRecord, PREAD_PROCESS_MEMORY_ROUTINE64 aReadMemoryRoutine,
      PFUNCTION_TABLE_ACCESS_ROUTINE64 aFunctionTableAccessRoutine,
      PGET_MODULE_BASE_ROUTINE64 aGetModuleBaseRoutine,
      PTRANSLATE_ADDRESS_ROUTINE64 aTranslateAddress) {
    if (!ReadyToUse()) {
      return FALSE;
    }

    AutoCriticalSection guard(&sCriticalSection);
    return ::StackWalk64(aMachineType, sSessionId, aThread, aStackFrame,
                         aContextRecord, aReadMemoryRoutine,
                         aFunctionTableAccessRoutine, aGetModuleBaseRoutine,
                         aTranslateAddress);
  }

 protected:
  enum class InitFlag : bool {
    BasicInit,
    WithSymbolSupport,
  };

  explicit inline DbgHelpWrapper(InitFlag initFlag) {
    mInitSuccess = Initialize(initFlag);
  }

  // DbgHelp functions are not thread-safe and should therefore be protected
  // by using this critical section through a AutoCriticalSection.
  static CRITICAL_SECTION sCriticalSection;

  // DbgHelp functions require a unique HANDLE hProcess that should be the same
  // throughout the current session. We refer to this handle as a session id.
  // Ideally the session id should be a valid HANDLE to the target process,
  // which in our case is the current process.
  //
  // However, in order to avoid conflicts with other sessions, the session id
  // should be unique and therefore not just GetCurrentProcess(), which other
  // pieces of code tend to already use (see bug 1699328).
  //
  // We therefore define sSessionId as a duplicate of the current process
  // handle, a solution that meets all the requirements listed above.
  static HANDLE sSessionId;

 private:
  bool mInitSuccess;

  // This function initializes sCriticalSection, sSessionId and loads DbgHelp.
  // It also calls SymInitialize if called with aInitFlag::WithSymbolSupport.
  // It is thread-safe and reentrancy-safe.
  [[nodiscard]] static bool Initialize(InitFlag aInitFlag);

  // In debug and fuzzing builds, MOZ_ASSERT and MOZ_CRASH walk the stack to
  // print it before actually crashing. This code path uses a DbgHelpWrapper
  // object, hence *any* MOZ_ASSERT or MOZ_CRASH failure reached from
  // Initialize() leads to rentrancy (see bug 1869997 for an example). Such
  // failures can occur indirectly when we load dbghelp.dll, because we
  // override various Microsoft-internal functions that are called upon DLL
  // loading. We protect against reentrancy by keeping track of the ID of the
  // thread that runs the initialization code.
  static Atomic<DWORD> sInitializationThreadId;
};

CRITICAL_SECTION DbgHelpWrapper::sCriticalSection{};
Atomic<DWORD> DbgHelpWrapper::sInitializationThreadId{0};
HANDLE DbgHelpWrapper::sSessionId{nullptr};

// Thread-safety here is ensured by the C++ standard: scoped static
// initialization is thread-safe. sInitializationThreadId is used to protect
// against reentrancy -- and only for that purpose.
[[nodiscard]] /* static */ bool DbgHelpWrapper::Initialize(
    DbgHelpWrapper::InitFlag aInitFlag) {
  // In the code below, it is only safe to reach MOZ_ASSERT or MOZ_CRASH while
  // sInitializationThreadId is set to the current thread id.
  static Atomic<DWORD> sInitializationThreadId{0};
  DWORD currentThreadId = ::GetCurrentThreadId();

  // This code relies on Windows never giving us a current thread ID of zero.
  // We make this assumption explicit, by failing if that should ever occur.
  if (!currentThreadId) {
    return false;
  }

  if (sInitializationThreadId == currentThreadId) {
    // This is a reentrant call and we must abort here.
    return false;
  }

  static const bool sHasInitializedDbgHelp = [currentThreadId]() {
    // Per the C++ standard, only one thread evers reaches this path.
    sInitializationThreadId = currentThreadId;

    ::InitializeCriticalSection(&sCriticalSection);

    if (!::DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                           GetCurrentProcess(), &sSessionId, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)) {
      return false;
    }

    bool dbgHelpLoaded = static_cast<bool>(::LoadLibraryW(L"dbghelp.dll"));

    MOZ_ASSERT(dbgHelpLoaded);
    sInitializationThreadId = 0;
    return dbgHelpLoaded;
  }();

  // If we don't need symbol initialization, we are done. If we need it, we
  // can only proceed if DbgHelp initialization was successful.
  if (aInitFlag == InitFlag::BasicInit || !sHasInitializedDbgHelp) {
    return sHasInitializedDbgHelp;
  }

  static const bool sHasInitializedSymbols = [currentThreadId]() {
    // Per the C++ standard, only one thread evers reaches this path.
    sInitializationThreadId = currentThreadId;

    bool symbolsInitialized = false;

    {
      AutoCriticalSection guard(&sCriticalSection);
      ::SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
      symbolsInitialized =
          static_cast<bool>(::SymInitializeW(sSessionId, nullptr, TRUE));
      /* XXX At some point we need to arrange to call SymCleanup */
    }

    if (!symbolsInitialized) {
      PrintError("SymInitialize");
    }

    MOZ_ASSERT(symbolsInitialized);
    sInitializationThreadId = 0;
    return symbolsInitialized;
  }();

  return sHasInitializedSymbols;
}

// Some APIs such as SymFromAddr also require that the session id has gone
// through SymInitialize. This is handled by child class DbgHelpWrapperSym.
class DbgHelpWrapperSym : public DbgHelpWrapper {
 public:
  explicit DbgHelpWrapperSym() : DbgHelpWrapper(InitFlag::WithSymbolSupport) {}

  inline BOOL SymFromAddr(DWORD64 aAddress, PDWORD64 aDisplacement,
                          PSYMBOL_INFO aSymbol) {
    if (!ReadyToUse()) {
      return FALSE;
    }

    AutoCriticalSection guard(&sCriticalSection);
    return ::SymFromAddr(sSessionId, aAddress, aDisplacement, aSymbol);
  }

  BOOL SymGetModuleInfoEspecial64(DWORD64 aAddr, PIMAGEHLP_MODULE64 aModuleInfo,
                                  PIMAGEHLP_LINE64 aLineInfo);

 private:
  // Helpers for SymGetModuleInfoEspecial64
  struct CallbackEspecial64UserContext {
    HANDLE mSessionId;
    DWORD64 mAddr;
  };

  static BOOL CALLBACK CallbackEspecial64(PCSTR aModuleName,
                                          DWORD64 aModuleBase,
                                          ULONG aModuleSize,
                                          PVOID aUserContext);
};

// Wrapper around a reference to a CONTEXT, to simplify access to main
// platform-specific execution registers.
// It also avoids using CONTEXT* nullable pointers.
class CONTEXTGenericAccessors {
 public:
  explicit CONTEXTGenericAccessors(CONTEXT& aCONTEXT) : mCONTEXT(aCONTEXT) {}

  CONTEXT* CONTEXTPtr() { return &mCONTEXT; }

  inline auto& PC() {
#  if defined(_M_AMD64)
    return mCONTEXT.Rip;
#  elif defined(_M_ARM64)
    return mCONTEXT.Pc;
#  elif defined(_M_IX86)
    return mCONTEXT.Eip;
#  else
#    error "unknown platform"
#  endif
  }

  inline auto& SP() {
#  if defined(_M_AMD64)
    return mCONTEXT.Rsp;
#  elif defined(_M_ARM64)
    return mCONTEXT.Sp;
#  elif defined(_M_IX86)
    return mCONTEXT.Esp;
#  else
#    error "unknown platform"
#  endif
  }

  inline auto& BP() {
#  if defined(_M_AMD64)
    return mCONTEXT.Rbp;
#  elif defined(_M_ARM64)
    return mCONTEXT.Fp;
#  elif defined(_M_IX86)
    return mCONTEXT.Ebp;
#  else
#    error "unknown platform"
#  endif
  }

 private:
  CONTEXT& mCONTEXT;
};

/**
 * Walk the stack, translating PC's found into strings and recording the
 * chain in aBuffer. For this to work properly, the DLLs must be rebased
 * so that the address in the file agrees with the address in memory.
 * Otherwise StackWalk will return FALSE when it hits a frame in a DLL
 * whose in memory address doesn't match its in-file address.
 */

static void DoMozStackWalkThread(MozWalkStackCallback aCallback,
                                 const void* aFirstFramePC, uint32_t aMaxFrames,
                                 void* aClosure, HANDLE aThread,
                                 CONTEXT* aContext) {
#  if defined(_M_IX86)
  DbgHelpWrapper dbgHelp;
  if (!dbgHelp.ReadyToUse()) {
    return;
  }
#  endif

  HANDLE targetThread = aThread;
  bool walkCallingThread;
  if (!targetThread) {
    targetThread = ::GetCurrentThread();
    walkCallingThread = true;
  } else {
    DWORD targetThreadId = ::GetThreadId(targetThread);
    DWORD currentThreadId = ::GetCurrentThreadId();
    walkCallingThread = (targetThreadId == currentThreadId);
  }

  // If not already provided, get a context for the specified thread.
  CONTEXT context_buf;
  if (!aContext) {
    memset(&context_buf, 0, sizeof(CONTEXT));
    context_buf.ContextFlags = CONTEXT_FULL;
    if (walkCallingThread) {
      ::RtlCaptureContext(&context_buf);
    } else if (!GetThreadContext(targetThread, &context_buf)) {
      return;
    }
  }
  CONTEXTGenericAccessors context{aContext ? *aContext : context_buf};

#  if defined(_M_IX86)
  // Setup initial stack frame to walk from.
  STACKFRAME64 frame64;
  memset(&frame64, 0, sizeof(frame64));
  frame64.AddrPC.Offset = context.PC();
  frame64.AddrStack.Offset = context.SP();
  frame64.AddrFrame.Offset = context.BP();
  frame64.AddrPC.Mode = AddrModeFlat;
  frame64.AddrStack.Mode = AddrModeFlat;
  frame64.AddrFrame.Mode = AddrModeFlat;
  frame64.AddrReturn.Mode = AddrModeFlat;
#  endif

#  if defined(_M_AMD64) || defined(_M_ARM64)
  // If at least one thread (we don't know which) may be holding a lock that
  // can deadlock RtlLookupFunctionEntry, we can't proceed because that thread
  // may be the one that we're trying to walk the stack of.
  //
  // But if there is no such thread by this point, then our target thread can't
  // be holding a lock, so it's safe to proceed. By virtue of being suspended,
  // the target thread can't acquire any new locks during our stack walking, so
  // we only need to do this check once. Other threads may temporarily acquire
  // the locks while we're walking the stack, but that's mostly fine -- calling
  // RtlLookupFunctionEntry will make us wait for them to release the locks,
  // but at least we won't deadlock.
  if (!IsStackWalkingSafe()) {
    return;
  }

  bool firstFrame = true;
#  endif

  FrameSkipper skipper(aFirstFramePC);

  uint32_t frames = 0;

  // Now walk the stack.
  while (true) {
    DWORD64 addr;
    DWORD64 spaddr;

#  if defined(_M_IX86)
    // 32-bit frame unwinding.
    BOOL ok = dbgHelp.StackWalk64(
        IMAGE_FILE_MACHINE_I386, targetThread, &frame64, context.CONTEXTPtr(),
        nullptr,
        ::SymFunctionTableAccess64,  // function table access routine
        ::SymGetModuleBase64,        // module base routine
        0);

    if (ok) {
      addr = frame64.AddrPC.Offset;
      spaddr = frame64.AddrStack.Offset;
    } else {
      addr = 0;
      spaddr = 0;
      if (walkCallingThread) {
        PrintError("WalkStack64");
      }
    }

    if (!ok) {
      break;
    }

#  elif defined(_M_AMD64) || defined(_M_ARM64)

    auto currentInstr = context.PC();

    // If we reach a frame in JIT code, we don't have enough information to
    // unwind, so we have to give up.
    if (sJitCodeRegionStart && (uint8_t*)currentInstr >= sJitCodeRegionStart &&
        (uint8_t*)currentInstr < sJitCodeRegionStart + sJitCodeRegionSize) {
      break;
    }

    // We must also avoid msmpeg2vdec.dll's JIT region: they don't generate
    // unwind data, so their JIT unwind callback just throws up its hands and
    // terminates the process.
    if (sMsMpegJitCodeRegionStart &&
        (uint8_t*)currentInstr >= sMsMpegJitCodeRegionStart &&
        (uint8_t*)currentInstr <
            sMsMpegJitCodeRegionStart + sMsMpegJitCodeRegionSize) {
      break;
    }

    // 64-bit frame unwinding.
    // Try to look up unwind metadata for the current function.
    ULONG64 imageBase;
    PRUNTIME_FUNCTION runtimeFunction =
        RtlLookupFunctionEntry(currentInstr, &imageBase, NULL);

    if (runtimeFunction) {
      PVOID dummyHandlerData;
      ULONG64 dummyEstablisherFrame;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, currentInstr,
                       runtimeFunction, context.CONTEXTPtr(), &dummyHandlerData,
                       &dummyEstablisherFrame, nullptr);
    } else if (firstFrame) {
      // Leaf functions can be unwound by hand.
      context.PC() = *reinterpret_cast<DWORD64*>(context.SP());
      context.SP() += sizeof(void*);
    } else {
      // Something went wrong.
      break;
    }

    addr = context.PC();
    spaddr = context.SP();
    firstFrame = false;
#  else
#    error "unknown platform"
#  endif

    if (addr == 0) {
      break;
    }

    if (skipper.ShouldSkipPC((void*)addr)) {
      continue;
    }

    aCallback(++frames, (void*)addr, (void*)spaddr, aClosure);

    if (aMaxFrames != 0 && frames == aMaxFrames) {
      break;
    }

#  if defined(_M_IX86)
    if (frame64.AddrReturn.Offset == 0) {
      break;
    }
#  endif
  }
}

MFBT_API void MozStackWalkThread(MozWalkStackCallback aCallback,
                                 uint32_t aMaxFrames, void* aClosure,
                                 HANDLE aThread, CONTEXT* aContext) {
  // We don't pass a aFirstFramePC because we walk the stack for another
  // thread.
  DoMozStackWalkThread(aCallback, nullptr, aMaxFrames, aClosure, aThread,
                       aContext);
}

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {
  DoMozStackWalkThread(aCallback, aFirstFramePC ? aFirstFramePC : CallerPC(),
                       aMaxFrames, aClosure, nullptr, nullptr);
}

/* static */ BOOL CALLBACK
DbgHelpWrapperSym::CallbackEspecial64(PCSTR aModuleName, DWORD64 aModuleBase,
                                      ULONG aModuleSize, PVOID aUserContext) {
  BOOL retval = TRUE;
  auto context = reinterpret_cast<CallbackEspecial64UserContext*>(aUserContext);
  DWORD64 addr = context->mAddr;

  /*
   * You'll want to control this if we are running on an
   *  architecture where the addresses go the other direction.
   * Not sure this is even a realistic consideration.
   */
  const BOOL addressIncreases = TRUE;

  /*
   * If it falls in side the known range, load the symbols.
   */
  if (addressIncreases
          ? (addr >= aModuleBase && addr <= (aModuleBase + aModuleSize))
          : (addr <= aModuleBase && addr >= (aModuleBase - aModuleSize))) {
    retval = !!::SymLoadModule64(context->mSessionId, nullptr, aModuleName,
                                 nullptr, aModuleBase, aModuleSize);
    if (!retval) {
      PrintError("SymLoadModule64");
    }
  }

  return retval;
}

/*
 * SymGetModuleInfoEspecial
 *
 * Attempt to determine the module information.
 * Bug 112196 says this DLL may not have been loaded at the time
 *  SymInitialize was called, and thus the module information
 *  and symbol information is not available.
 * This code rectifies that problem.
 */

// New members were added to IMAGEHLP_MODULE64 (that show up in the
// Platform SDK that ships with VC8, but not the Platform SDK that ships
// with VC7.1, i.e., between DbgHelp 6.0 and 6.1), but we don't need to
// use them, and it's useful to be able to function correctly with the
// older library.  (Stock Windows XP SP2 seems to ship with dbghelp.dll
// version 5.1.)  Since Platform SDK version need not correspond to
// compiler version, and the version number in debughlp.h was NOT bumped
// when these changes were made, ifdef based on a constant that was
// added between these versions.
#  ifdef SSRVOPT_SETCONTEXT
#    define NS_IMAGEHLP_MODULE64_SIZE                                        \
      (((offsetof(IMAGEHLP_MODULE64, LoadedPdbName) + sizeof(DWORD64) - 1) / \
        sizeof(DWORD64)) *                                                   \
       sizeof(DWORD64))
#  else
#    define NS_IMAGEHLP_MODULE64_SIZE sizeof(IMAGEHLP_MODULE64)
#  endif

BOOL DbgHelpWrapperSym::SymGetModuleInfoEspecial64(
    DWORD64 aAddr, PIMAGEHLP_MODULE64 aModuleInfo, PIMAGEHLP_LINE64 aLineInfo) {
  if (!ReadyToUse()) {
    return FALSE;
  }

  AutoCriticalSection guard(&sCriticalSection);
  BOOL retval = FALSE;

  /*
   * Init the vars if we have em.
   */
  aModuleInfo->SizeOfStruct = NS_IMAGEHLP_MODULE64_SIZE;
  if (aLineInfo) {
    aLineInfo->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
  }

  /*
   * Give it a go.
   * It may already be loaded.
   */
  retval = ::SymGetModuleInfo64(sSessionId, aAddr, aModuleInfo);
  if (retval == FALSE) {
    /*
     * Not loaded, here's the magic.
     * Go through all the modules.
     */
    CallbackEspecial64UserContext context{
        .mSessionId = sSessionId,
        .mAddr = aAddr,
    };
    BOOL enumRes = ::EnumerateLoadedModules64(
        sSessionId, CallbackEspecial64, reinterpret_cast<PVOID>(&context));
    if (enumRes != FALSE) {
      /*
       * One final go.
       * If it fails, then well, we have other problems.
       */
      retval = ::SymGetModuleInfo64(sSessionId, aAddr, aModuleInfo);
    }
  }

  /*
   * If we got module info, we may attempt line info as well.
   * We will not report failure if this does not work.
   */
  if (retval != FALSE && aLineInfo) {
    DWORD displacement = 0;
    BOOL lineRes = FALSE;
    lineRes =
        ::SymGetLineFromAddr64(sSessionId, aAddr, &displacement, aLineInfo);
    if (!lineRes) {
      // Clear out aLineInfo to indicate that it's not valid
      memset(aLineInfo, 0, sizeof(*aLineInfo));
    }
  }

  return retval;
}

MFBT_API bool MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  DbgHelpWrapperSym dbgHelp;
  if (!dbgHelp.ReadyToUse()) {
    return false;
  }

  // Attempt to load module info before we attempt to resolve the symbol.
  // This just makes sure we get good info if available.
  DWORD64 addr = (DWORD64)aPC;
  IMAGEHLP_MODULE64 modInfo;
  IMAGEHLP_LINE64 lineInfo;
  BOOL modInfoRes;
  modInfoRes = dbgHelp.SymGetModuleInfoEspecial64(addr, &modInfo, &lineInfo);

  if (modInfoRes) {
    strncpy(aDetails->library, modInfo.LoadedImageName,
            sizeof(aDetails->library));
    aDetails->library[std::size(aDetails->library) - 1] = '\0';
    aDetails->loffset = (char*)aPC - (char*)modInfo.BaseOfImage;

    if (lineInfo.FileName) {
      strncpy(aDetails->filename, lineInfo.FileName,
              sizeof(aDetails->filename));
      aDetails->filename[std::size(aDetails->filename) - 1] = '\0';
      aDetails->lineno = lineInfo.LineNumber;
    }
  }

  ULONG64 buffer[(sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR) +
                  sizeof(ULONG64) - 1) /
                 sizeof(ULONG64)];
  PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = MAX_SYM_NAME;

  DWORD64 displacement;
  BOOL ok = dbgHelp.SymFromAddr(addr, &displacement, pSymbol);

  if (ok) {
    strncpy(aDetails->function, pSymbol->Name, sizeof(aDetails->function));
    aDetails->function[std::size(aDetails->function) - 1] = '\0';
    aDetails->foffset = static_cast<ptrdiff_t>(displacement);
  }

  return true;
}

// i386 or PPC Linux stackwalking code
//
// Changes to to OS/Architecture support here should be reflected in
// build/moz.configure/memory.configure
#elif HAVE_DLADDR &&                                           \
    (HAVE__UNWIND_BACKTRACE || MOZ_STACKWALK_SUPPORTS_LINUX || \
     MOZ_STACKWALK_SUPPORTS_MACOSX)

#  include <stdlib.h>
#  include <stdio.h>

// On glibc 2.1, the Dl_info api defined in <dlfcn.h> is only exposed
// if __USE_GNU is defined.  I suppose its some kind of standards
// adherence thing.
//
#  if (__GLIBC_MINOR__ >= 1) && !defined(__USE_GNU)
#    define __USE_GNU
#  endif

// This thing is exported by libstdc++
// Yes, this is a gcc only hack
#  if defined(MOZ_DEMANGLE_SYMBOLS)
#    include <cxxabi.h>
#  endif  // MOZ_DEMANGLE_SYMBOLS

namespace mozilla {

void DemangleSymbol(const char* aSymbol, char* aBuffer, int aBufLen) {
  aBuffer[0] = '\0';

#  if defined(MOZ_DEMANGLE_SYMBOLS)
  /* See demangle.h in the gcc source for the voodoo */
  char* demangled = abi::__cxa_demangle(aSymbol, 0, 0, 0);

  if (demangled) {
    strncpy(aBuffer, demangled, aBufLen);
    aBuffer[aBufLen - 1] = '\0';
    free(demangled);
  }
#  endif  // MOZ_DEMANGLE_SYMBOLS
}

}  // namespace mozilla

// {x86, ppc} x {Linux, Mac} stackwalking code.
//
// Changes to to OS/Architecture support here should be reflected in
// build/moz.configure/memory.configure
#  if ((defined(__i386) || defined(PPC) || defined(__ppc__)) && \
       (MOZ_STACKWALK_SUPPORTS_MACOSX || MOZ_STACKWALK_SUPPORTS_LINUX))

static void DoFramePointerStackWalk(MozWalkStackCallback aCallback,
                                    const void* aFirstFramePC,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd);

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {
  // Get the frame pointer
  void** bp = (void**)__builtin_frame_address(0);

  void* stackEnd;
#    if HAVE___LIBC_STACK_END
  stackEnd = __libc_stack_end;
#    elif defined(XP_DARWIN)
  stackEnd = pthread_get_stackaddr_np(pthread_self());
#    elif defined(ANDROID)
  pthread_attr_t sattr;
  pthread_attr_init(&sattr);
  int rc = pthread_getattr_np(pthread_self(), &sattr);
  MOZ_RELEASE_ASSERT(rc == 0, "pthread_getattr_np failed");
  void* stackBase = stackEnd = nullptr;
  size_t stackSize = 0;
  if (gettid() != getpid()) {
    // bionic's pthread_attr_getstack doesn't tell the truth for the main
    // thread (see bug 846670). So don't use it for the main thread.
    if (!pthread_attr_getstack(&sattr, &stackBase, &stackSize)) {
      stackEnd = static_cast<char*>(stackBase) + stackSize;
    } else {
      stackEnd = nullptr;
    }
  }
  if (!stackEnd) {
    // So consider the current frame pointer + an arbitrary size of 8MB
    // (modulo overflow ; not really arbitrary as it's the default stack
    // size for the main thread) if pthread_attr_getstack failed for
    // some reason (or was skipped).
    static const uintptr_t kMaxStackSize = 8 * 1024 * 1024;
    uintptr_t maxStackStart = uintptr_t(-1) - kMaxStackSize;
    uintptr_t stackStart = std::max(maxStackStart, uintptr_t(bp));
    stackEnd = reinterpret_cast<void*>(stackStart + kMaxStackSize);
  }
#    else
#      error Unsupported configuration
#    endif
  DoFramePointerStackWalk(aCallback, aFirstFramePC, aMaxFrames, aClosure, bp,
                          stackEnd);
}

#  elif defined(HAVE__UNWIND_BACKTRACE)

// libgcc_s.so symbols _Unwind_Backtrace@@GCC_3.3 and _Unwind_GetIP@@GCC_3.0
#    include <unwind.h>

struct unwind_info {
  MozWalkStackCallback callback;
  FrameSkipper skipper;
  int maxFrames;
  int numFrames;
  void* closure;
};

static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context* context,
                                           void* closure) {
  _Unwind_Reason_Code ret = _URC_NO_REASON;
  unwind_info* info = static_cast<unwind_info*>(closure);
  void* pc = reinterpret_cast<void*>(_Unwind_GetIP(context));
#    if HAVE___LIBC_STACK_END && defined(__aarch64__)
  // Work around https://sourceware.org/bugzilla/show_bug.cgi?id=32612
  // The _dl_tlsdesc_dynamic function can't be unwound through with
  // _Unwind_Backtrace when glibc is built with aarch64 PAC (that leads
  // to a crash).
  // Unfortunately, we can't get the address of that specific function, so
  // we just disallow all of ld-linux-aarch64.so.1: when we hit an address
  // in there, we make _Unwind_Backtrace stop.
  // In the case of _dl_tlsdesc_dynamic, this would stop the stackwalk at
  // tls_get_addr_tail, which is enough information to know the stack comes
  // from ld.so, and we even get inlining info giving us malloc,
  // allocate_dtv_entry and allocate_and_init, which is plenty enough and
  // better than nothing^Hcrashing.
  // To figure out whether the frame falls into ld-linux-aarch64.so.1, we
  // use __libc_stack_end (which lives there and is .data) as upper bound
  // (assuming .data comes after .text), and get the base address of the
  // library via dladdr.
  if (!ldso_base) {
    Dl_info info;
    dladdr(&__libc_stack_end, &info);
    ldso_base = (uintptr_t)info.dli_fbase;
  }
  if (ldso_base && ((uintptr_t)pc > ldso_base) &&
      (uintptr_t)pc < (uintptr_t)&__libc_stack_end) {
    // Any error code will do, we just want to stop the walk even when
    // we haven't reached the limit.
    ret = _URC_FOREIGN_EXCEPTION_CAUGHT;
  }
#    endif
  // TODO Use something like '_Unwind_GetGR()' to get the stack pointer.
  if (!info->skipper.ShouldSkipPC(pc)) {
    info->numFrames++;
    (*info->callback)(info->numFrames, pc, nullptr, info->closure);
    if (info->maxFrames != 0 && info->numFrames == info->maxFrames) {
      // Again, any error code that stops the walk will do.
      return _URC_FOREIGN_EXCEPTION_CAUGHT;
    }
  }
  return ret;
}

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {
  unwind_info info;
  info.callback = aCallback;
  info.skipper = FrameSkipper(aFirstFramePC ? aFirstFramePC : CallerPC());
  info.maxFrames = aMaxFrames;
  info.numFrames = 0;
  info.closure = aClosure;

  // We ignore the return value from _Unwind_Backtrace. There are three main
  // reasons for this.
  // - On ARM/Android bionic's _Unwind_Backtrace usually (always?) returns
  //   _URC_FAILURE.  See
  //   https://bugzilla.mozilla.org/show_bug.cgi?id=717853#c110.
  // - If aMaxFrames != 0, we want to stop early, and the only way to do that
  //   is to make unwind_callback return something other than _URC_NO_REASON,
  //   which causes _Unwind_Backtrace to return a non-success code.
  // - MozStackWalk doesn't have a return value anyway.
  (void)_Unwind_Backtrace(unwind_callback, &info);
}

#  endif

bool MFBT_API MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  Dl_info info;

#  if defined(ANDROID) && defined(MOZ_LINKER)
  int ok = __wrap_dladdr(aPC, &info);
#  else
  int ok = dladdr(aPC, &info);
#  endif

  if (!ok) {
    return true;
  }

  strncpy(aDetails->library, info.dli_fname, sizeof(aDetails->library));
  aDetails->library[std::size(aDetails->library) - 1] = '\0';
  aDetails->loffset = (char*)aPC - (char*)info.dli_fbase;

#  if !defined(XP_FREEBSD)
  // On FreeBSD, dli_sname is unusably bad, it often returns things like
  // 'gtk_xtbin_new' or 'XRE_GetBootstrap' instead of long C++ symbols. Just let
  // GetFunction do the lookup directly in the ELF image.

  const char* symbol = info.dli_sname;
  if (!symbol || symbol[0] == '\0') {
    return true;
  }

  DemangleSymbol(symbol, aDetails->function, sizeof(aDetails->function));

  if (aDetails->function[0] == '\0') {
    // Just use the mangled symbol if demangling failed.
    strncpy(aDetails->function, symbol, sizeof(aDetails->function));
    aDetails->function[std::size(aDetails->function) - 1] = '\0';
  }

  aDetails->foffset = (char*)aPC - (char*)info.dli_saddr;
#  endif

  return true;
}

#else  // unsupported platform.

MFBT_API void MozStackWalk(MozWalkStackCallback aCallback,
                           const void* aFirstFramePC, uint32_t aMaxFrames,
                           void* aClosure) {}

MFBT_API bool MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;
  return false;
}

#endif

#if defined(XP_WIN) || defined(XP_MACOSX) || defined(XP_LINUX)

#  if defined(XP_MACOSX) && defined(__aarch64__)
// On macOS arm64, system libraries are arm64e binaries, and arm64e can do
// pointer authentication: The low bits of the pointer are the actual pointer
// value, and the high bits are an encrypted hash. During stackwalking, we need
// to strip off this hash. In theory, ptrauth_strip would be the right function
// to call for this. However, that function is a no-op unless it's called from
// code which also builds as arm64e - which we do not. So we cannot use it. So
// for now, we hardcode a mask that seems to work today: 40 bits for the pointer
// and 24 bits for the hash seems to do the trick. We can worry about
// dynamically computing the correct mask if this ever stops working.
const uintptr_t kPointerMask =
    (uintptr_t(1) << 40) - 1;  // 40 bits pointer, 24 bit PAC
#  else
const uintptr_t kPointerMask = ~uintptr_t(0);
#  endif

MOZ_ASAN_IGNORE
static void DoFramePointerStackWalk(MozWalkStackCallback aCallback,
                                    const void* aFirstFramePC,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {
  // Stack walking code courtesy Kipp's "leaky".

  FrameSkipper skipper(aFirstFramePC);
  uint32_t numFrames = 0;

  // Sanitize the given aBp. Assume that something reasonably close to
  // but before the stack end is going be a valid frame pointer. Also
  // check that it is an aligned address. This increases the chances
  // that if the pointer is not valid (which might happen if the caller
  // called __builtin_frame_address(1) and its frame is busted for some
  // reason), we won't read it, leading to a crash. Because the calling
  // code is not using frame pointers when returning, it might actually
  // recover just fine.
  static const uintptr_t kMaxStackSize = 8 * 1024 * 1024;
  if (uintptr_t(aBp) < uintptr_t(aStackEnd) -
                           std::min(kMaxStackSize, uintptr_t(aStackEnd)) ||
      aBp >= aStackEnd || (uintptr_t(aBp) & 3)) {
    return;
  }

  while (aBp) {
    void** next = (void**)*aBp;
    // aBp may not be a frame pointer on i386 if code was compiled with
    // -fomit-frame-pointer, so do some sanity checks.
    // (aBp should be a frame pointer on ppc(64) but checking anyway may help
    // a little if the stack has been corrupted.)
    // We don't need to check against the begining of the stack because
    // we can assume that aBp > sp
    if (next <= aBp || next >= aStackEnd || (uintptr_t(next) & 3)) {
      break;
    }
#  if (defined(__ppc__) && defined(XP_MACOSX)) || defined(__powerpc64__)
    // ppc mac or powerpc64 linux
    void* pc = *(aBp + 2);
    aBp += 3;
#  else  // i386 or powerpc32 linux
    void* pc = *(aBp + 1);
    aBp += 2;
#  endif

    // Strip off pointer authentication hash, if present. For now, it looks
    // like only return addresses require stripping, and stack pointers do
    // not. This might change in the future.
    pc = (void*)((uintptr_t)pc & kPointerMask);

    if (!skipper.ShouldSkipPC(pc)) {
      // Assume that the SP points to the BP of the function
      // it called. We can't know the exact location of the SP
      // but this should be sufficient for our use the SP
      // to order elements on the stack.
      numFrames++;
      (*aCallback)(numFrames, pc, aBp, aClosure);
      if (aMaxFrames != 0 && numFrames == aMaxFrames) {
        break;
      }
    }
    aBp = next;
  }
}

namespace mozilla {

MFBT_API void FramePointerStackWalk(MozWalkStackCallback aCallback,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {
  // We don't pass a aFirstFramePC because we start walking the stack from the
  // frame at aBp.
  DoFramePointerStackWalk(aCallback, nullptr, aMaxFrames, aClosure, aBp,
                          aStackEnd);
}

}  // namespace mozilla

#else

namespace mozilla {
MFBT_API void FramePointerStackWalk(MozWalkStackCallback aCallback,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {}
}  // namespace mozilla

#endif

MFBT_API int MozFormatCodeAddressDetails(
    char* aBuffer, uint32_t aBufferSize, uint32_t aFrameNumber, void* aPC,
    const MozCodeAddressDetails* aDetails) {
  return MozFormatCodeAddress(aBuffer, aBufferSize, aFrameNumber, aPC,
                              aDetails->function, aDetails->library,
                              aDetails->loffset, aDetails->filename,
                              aDetails->lineno);
}

MFBT_API int MozFormatCodeAddress(char* aBuffer, uint32_t aBufferSize,
                                  uint32_t aFrameNumber, const void* aPC,
                                  const char* aFunction, const char* aLibrary,
                                  ptrdiff_t aLOffset, const char* aFileName,
                                  uint32_t aLineNo) {
  const char* function = aFunction && aFunction[0] ? aFunction : "???";
  if (aFileName && aFileName[0]) {
    // We have a filename and (presumably) a line number. Use them.
    return SprintfBuf(aBuffer, aBufferSize, "#%02u: %s (%s:%u)", aFrameNumber,
                      function, aFileName, aLineNo);
  } else if (aLibrary && aLibrary[0]) {
    // We have no filename, but we do have a library name. Use it and the
    // library offset, and print them in a way that `fix_stacks.py` can
    // post-process.
    return SprintfBuf(aBuffer, aBufferSize, "#%02u: %s[%s +0x%" PRIxPTR "]",
                      aFrameNumber, function, aLibrary,
                      static_cast<uintptr_t>(aLOffset));
  } else {
    // We have nothing useful to go on. (The format string is split because
    // '??)' is a trigraph and causes a warning, sigh.)
    return SprintfBuf(aBuffer, aBufferSize,
                      "#%02u: ??? (???:???"
                      ")",
                      aFrameNumber);
  }
}

static void EnsureWrite(FILE* aStream, const char* aBuf, size_t aLen) {
#ifdef XP_WIN
  int fd = _fileno(aStream);
#else
  int fd = fileno(aStream);
#endif
  while (aLen > 0) {
#ifdef XP_WIN
    auto written = _write(fd, aBuf, aLen);
#else
    auto written = write(fd, aBuf, aLen);
#endif
    if (written <= 0 || size_t(written) > aLen) {
      break;
    }
    aBuf += written;
    aLen -= written;
  }
}

template <int N>
static int PrintStackFrameBuf(char (&aBuf)[N], uint32_t aFrameNumber, void* aPC,
                              void* aSP) {
  MozCodeAddressDetails details;
  MozDescribeCodeAddress(aPC, &details);
  int len =
      MozFormatCodeAddressDetails(aBuf, N - 1, aFrameNumber, aPC, &details);
  len = std::min(len, N - 2);
  aBuf[len++] = '\n';
  aBuf[len] = '\0';
  return len;
}

static void PrintStackFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                            void* aClosure) {
  FILE* stream = (FILE*)aClosure;
  char buf[1025];  // 1024 + 1 for trailing '\n'
  int len = PrintStackFrameBuf(buf, aFrameNumber, aPC, aSP);
  fflush(stream);
  EnsureWrite(stream, buf, len);
}

static bool WalkTheStackEnabled() {
  static bool result = [] {
    char* value = getenv("MOZ_DISABLE_WALKTHESTACK");
    return !(value && value[0]);
  }();
  return result;
}

MFBT_API void MozWalkTheStack(FILE* aStream, const void* aFirstFramePC,
                              uint32_t aMaxFrames) {
  if (WalkTheStackEnabled()) {
    MozStackWalk(PrintStackFrame, aFirstFramePC ? aFirstFramePC : CallerPC(),
                 aMaxFrames, aStream);
  }
}

static void WriteStackFrame(uint32_t aFrameNumber, void* aPC, void* aSP,
                            void* aClosure) {
  auto writer = (void (*)(const char*))aClosure;
  char buf[1024];
  PrintStackFrameBuf(buf, aFrameNumber, aPC, aSP);
  writer(buf);
}

MFBT_API void MozWalkTheStackWithWriter(void (*aWriter)(const char*),
                                        const void* aFirstFramePC,
                                        uint32_t aMaxFrames) {
  if (WalkTheStackEnabled()) {
    MozStackWalk(WriteStackFrame, aFirstFramePC ? aFirstFramePC : CallerPC(),
                 aMaxFrames, (void*)aWriter);
  }
}
