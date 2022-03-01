/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* API for getting a stack trace of the C/C++ stack on the current thread */

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/IntegerPrintfMacros.h"
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

#if defined(HAVE_DLOPEN) || defined(XP_DARWIN)
#  include <dlfcn.h>
#endif

#if (defined(XP_DARWIN) && \
     (defined(__i386) || defined(__ppc__) || defined(HAVE__UNWIND_BACKTRACE)))
#  define MOZ_STACKWALK_SUPPORTS_MACOSX 1
#else
#  define MOZ_STACKWALK_SUPPORTS_MACOSX 0
#endif

#if (defined(linux) &&                                            \
     ((defined(__GNUC__) && (defined(__i386) || defined(PPC))) || \
      defined(HAVE__UNWIND_BACKTRACE)))
#  define MOZ_STACKWALK_SUPPORTS_LINUX 1
#else
#  define MOZ_STACKWALK_SUPPORTS_LINUX 0
#endif

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 1)
#  define HAVE___LIBC_STACK_END 1
#else
#  define HAVE___LIBC_STACK_END 0
#endif

#if HAVE___LIBC_STACK_END
extern MOZ_EXPORT void* __libc_stack_end;  // from ld-linux.so
#endif

#ifdef ANDROID
#  include <algorithm>
#  include <unistd.h>
#  include <pthread.h>
#endif

class FrameSkipper {
 public:
  constexpr FrameSkipper() : mPc(0) {}
  bool ShouldSkipPC(void* aPC) {
    // Skip frames until we encounter the one we were initialized with,
    // and then never skip again.
    if (mPc != 0) {
      if (mPc != uintptr_t(aPC)) {
        return true;
      }
      mPc = 0;
    }
    return false;
  }
  explicit FrameSkipper(const void* aPc) : mPc(uintptr_t(aPc)) {}

 private:
  uintptr_t mPc;
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

CRITICAL_SECTION gDbgHelpCS;

#  if defined(_M_AMD64) || defined(_M_ARM64)
// Because various Win64 APIs acquire function-table locks, we need a way of
// preventing stack walking while those APIs are being called. Otherwise, the
// stack walker may suspend a thread holding such a lock, and deadlock when the
// stack unwind code attempts to wait for that lock.
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

void DesuppressStackWalking() { --sStackWalkSuppressions; }

MFBT_API
AutoSuppressStackWalking::AutoSuppressStackWalking() { SuppressStackWalking(); }

MFBT_API
AutoSuppressStackWalking::~AutoSuppressStackWalking() {
  DesuppressStackWalking();
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

static void InitializeDbgHelpCriticalSection() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  ::InitializeCriticalSection(&gDbgHelpCS);
  initialized = true;
}

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
  InitializeDbgHelpCriticalSection();

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
  // If there are any active suppressions, then at least one thread (we don't
  // know which) is holding a lock that can deadlock RtlVirtualUnwind. Since
  // that thread may be the one that we're trying to unwind, we can't proceed.
  //
  // But if there are no suppressions, then our target thread can't be holding
  // a lock, and it's safe to proceed. By virtue of being suspended, the target
  // thread can't acquire any new locks during the unwind process, so we only
  // need to do this check once. After that, sStackWalkSuppressions can be
  // changed by other threads while we're unwinding, and that's fine because
  // we can't deadlock with those threads.
  if (sStackWalkSuppressions) {
    return;
  }
#  endif

#  if defined(_M_AMD64) || defined(_M_ARM64)
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
    // Debug routines are not threadsafe, so grab the lock.
    EnterCriticalSection(&gDbgHelpCS);
    BOOL ok = StackWalk64(
#    if defined _M_IX86
        IMAGE_FILE_MACHINE_I386,
#    endif
        ::GetCurrentProcess(), targetThread, &frame64, context.CONTEXTPtr(),
        nullptr,
        SymFunctionTableAccess64,  // function table access routine
        SymGetModuleBase64,        // module base routine
        0);
    LeaveCriticalSection(&gDbgHelpCS);

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

static BOOL CALLBACK callbackEspecial64(PCSTR aModuleName, DWORD64 aModuleBase,
                                        ULONG aModuleSize, PVOID aUserContext) {
  BOOL retval = TRUE;
  DWORD64 addr = *(DWORD64*)aUserContext;

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
    retval = !!SymLoadModule64(GetCurrentProcess(), nullptr, (PSTR)aModuleName,
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

BOOL SymGetModuleInfoEspecial64(HANDLE aProcess, DWORD64 aAddr,
                                PIMAGEHLP_MODULE64 aModuleInfo,
                                PIMAGEHLP_LINE64 aLineInfo) {
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
  retval = SymGetModuleInfo64(aProcess, aAddr, aModuleInfo);
  if (retval == FALSE) {
    /*
     * Not loaded, here's the magic.
     * Go through all the modules.
     */
    // Need to cast to PENUMLOADED_MODULES_CALLBACK64 because the
    // constness of the first parameter of
    // PENUMLOADED_MODULES_CALLBACK64 varies over SDK versions (from
    // non-const to const over time).  See bug 391848 and bug
    // 415426.
    BOOL enumRes = EnumerateLoadedModules64(
        aProcess, (PENUMLOADED_MODULES_CALLBACK64)callbackEspecial64,
        (PVOID)&aAddr);
    if (enumRes != FALSE) {
      /*
       * One final go.
       * If it fails, then well, we have other problems.
       */
      retval = SymGetModuleInfo64(aProcess, aAddr, aModuleInfo);
    }
  }

  /*
   * If we got module info, we may attempt line info as well.
   * We will not report failure if this does not work.
   */
  if (retval != FALSE && aLineInfo) {
    DWORD displacement = 0;
    BOOL lineRes = FALSE;
    lineRes = SymGetLineFromAddr64(aProcess, aAddr, &displacement, aLineInfo);
    if (!lineRes) {
      // Clear out aLineInfo to indicate that it's not valid
      memset(aLineInfo, 0, sizeof(*aLineInfo));
    }
  }

  return retval;
}

static bool EnsureSymInitialized() {
  static bool gInitialized = false;
  bool retStat;

  if (gInitialized) {
    return gInitialized;
  }

  InitializeDbgHelpCriticalSection();

  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  retStat = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  if (!retStat) {
    PrintError("SymInitialize");
  }

  gInitialized = retStat;
  /* XXX At some point we need to arrange to call SymCleanup */

  return retStat;
}

MFBT_API bool MozDescribeCodeAddress(void* aPC,
                                     MozCodeAddressDetails* aDetails) {
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  if (!EnsureSymInitialized()) {
    return false;
  }

  HANDLE myProcess = ::GetCurrentProcess();
  BOOL ok;

  // debug routines are not threadsafe, so grab the lock.
  EnterCriticalSection(&gDbgHelpCS);

  //
  // Attempt to load module info before we attempt to resolve the symbol.
  // This just makes sure we get good info if available.
  //

  DWORD64 addr = (DWORD64)aPC;
  IMAGEHLP_MODULE64 modInfo;
  IMAGEHLP_LINE64 lineInfo;
  BOOL modInfoRes;
  modInfoRes = SymGetModuleInfoEspecial64(myProcess, addr, &modInfo, &lineInfo);

  if (modInfoRes) {
    strncpy(aDetails->library, modInfo.LoadedImageName,
            sizeof(aDetails->library));
    aDetails->library[mozilla::ArrayLength(aDetails->library) - 1] = '\0';
    aDetails->loffset = (char*)aPC - (char*)modInfo.BaseOfImage;

    if (lineInfo.FileName) {
      strncpy(aDetails->filename, lineInfo.FileName,
              sizeof(aDetails->filename));
      aDetails->filename[mozilla::ArrayLength(aDetails->filename) - 1] = '\0';
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
  ok = SymFromAddr(myProcess, addr, &displacement, pSymbol);

  if (ok) {
    strncpy(aDetails->function, pSymbol->Name, sizeof(aDetails->function));
    aDetails->function[mozilla::ArrayLength(aDetails->function) - 1] = '\0';
    aDetails->foffset = static_cast<ptrdiff_t>(displacement);
  }

  LeaveCriticalSection(&gDbgHelpCS);  // release our lock
  return true;
}

// i386 or PPC Linux stackwalking code
#elif HAVE_DLADDR &&                                           \
    (HAVE__UNWIND_BACKTRACE || MOZ_STACKWALK_SUPPORTS_LINUX || \
     MOZ_STACKWALK_SUPPORTS_MACOSX)

#  include <stdlib.h>
#  include <string.h>
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
  pthread_getattr_np(pthread_self(), &sattr);
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
  unwind_info* info = static_cast<unwind_info*>(closure);
  void* pc = reinterpret_cast<void*>(_Unwind_GetIP(context));
  // TODO Use something like '_Unwind_GetGR()' to get the stack pointer.
  if (!info->skipper.ShouldSkipPC(pc)) {
    info->numFrames++;
    (*info->callback)(info->numFrames, pc, nullptr, info->closure);
    if (info->maxFrames != 0 && info->numFrames == info->maxFrames) {
      // Again, any error code that stops the walk will do.
      return _URC_FOREIGN_EXCEPTION_CAUGHT;
    }
  }
  return _URC_NO_REASON;
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
  aDetails->library[mozilla::ArrayLength(aDetails->library) - 1] = '\0';
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
    aDetails->function[mozilla::ArrayLength(aDetails->function) - 1] = '\0';
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

MOZ_ASAN_BLACKLIST
static void DoFramePointerStackWalk(MozWalkStackCallback aCallback,
                                    const void* aFirstFramePC,
                                    uint32_t aMaxFrames, void* aClosure,
                                    void** aBp, void* aStackEnd) {
  // Stack walking code courtesy Kipp's "leaky".

  FrameSkipper skipper(aFirstFramePC);
  uint32_t numFrames = 0;

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
