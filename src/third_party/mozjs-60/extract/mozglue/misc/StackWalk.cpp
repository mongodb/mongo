/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* API for getting a stack trace of the C/C++ stack on the current thread */

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/StackWalk.h"

#include <string.h>

using namespace mozilla;

// for _Unwind_Backtrace from libcxxrt or libunwind
// cxxabi.h from libcxxrt implicitly includes unwind.h first
#if defined(HAVE__UNWIND_BACKTRACE) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#if defined(HAVE_DLOPEN) || defined(XP_DARWIN)
#include <dlfcn.h>
#endif

#if (defined(XP_DARWIN) && \
     (defined(__i386) || defined(__ppc__) || defined(HAVE__UNWIND_BACKTRACE)))
#define MOZ_STACKWALK_SUPPORTS_MACOSX 1
#else
#define MOZ_STACKWALK_SUPPORTS_MACOSX 0
#endif

#if (defined(linux) && \
     ((defined(__GNUC__) && (defined(__i386) || defined(PPC))) || \
      defined(HAVE__UNWIND_BACKTRACE)))
#define MOZ_STACKWALK_SUPPORTS_LINUX 1
#else
#define MOZ_STACKWALK_SUPPORTS_LINUX 0
#endif

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 1)
#define HAVE___LIBC_STACK_END 1
#else
#define HAVE___LIBC_STACK_END 0
#endif

#if HAVE___LIBC_STACK_END
extern MOZ_EXPORT void* __libc_stack_end; // from ld-linux.so
#endif

#ifdef ANDROID
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#endif

#if MOZ_STACKWALK_SUPPORTS_WINDOWS

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <malloc.h>
#include "mozilla/ArrayUtils.h"
#include "mozilla/Atomics.h"
#include "mozilla/StackWalk_windows.h"
#include "mozilla/WindowsVersion.h"

#include <imagehlp.h>
// We need a way to know if we are building for WXP (or later), as if we are, we
// need to use the newer 64-bit APIs. API_VERSION_NUMBER seems to fit the bill.
// A value of 9 indicates we want to use the new APIs.
#if API_VERSION_NUMBER < 9
#error Too old imagehlp.h
#endif

struct WalkStackData
{
  // Are we walking the stack of the calling thread? Note that we need to avoid
  // calling fprintf and friends if this is false, in order to avoid deadlocks.
  bool walkCallingThread;
  uint32_t skipFrames;
  HANDLE thread;
  HANDLE process;
  HANDLE eventStart;
  HANDLE eventEnd;
  void** pcs;
  uint32_t pc_size;
  uint32_t pc_count;
  uint32_t pc_max;
  void** sps;
  uint32_t sp_size;
  uint32_t sp_count;
  CONTEXT* context;
};

DWORD gStackWalkThread;
CRITICAL_SECTION gDbgHelpCS;

#ifdef _M_AMD64
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

MFBT_API
AutoSuppressStackWalking::AutoSuppressStackWalking()
{
  ++sStackWalkSuppressions;
}

MFBT_API
AutoSuppressStackWalking::~AutoSuppressStackWalking()
{
  --sStackWalkSuppressions;
}

static uint8_t* sJitCodeRegionStart;
static size_t sJitCodeRegionSize;
uint8_t* sMsMpegJitCodeRegionStart;
size_t sMsMpegJitCodeRegionSize;

MFBT_API void
RegisterJitCodeRegion(uint8_t* aStart, size_t aSize)
{
  // Currently we can only handle one JIT code region at a time
  MOZ_RELEASE_ASSERT(!sJitCodeRegionStart);

  sJitCodeRegionStart = aStart;
  sJitCodeRegionSize = aSize;
}

MFBT_API void
UnregisterJitCodeRegion(uint8_t* aStart, size_t aSize)
{
  // Currently we can only handle one JIT code region at a time
  MOZ_RELEASE_ASSERT(sJitCodeRegionStart &&
                     sJitCodeRegionStart == aStart &&
                     sJitCodeRegionSize == aSize);

  sJitCodeRegionStart = nullptr;
  sJitCodeRegionSize = 0;
}

#endif // _M_AMD64

// Routine to print an error message to standard error.
static void
PrintError(const char* aPrefix)
{
  LPSTR lpMsgBuf;
  DWORD lastErr = GetLastError();
  FormatMessageA(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,
    lastErr,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
    (LPSTR)&lpMsgBuf,
    0,
    nullptr
  );
  fprintf(stderr, "### ERROR: %s: %s",
          aPrefix, lpMsgBuf ? lpMsgBuf : "(null)\n");
  fflush(stderr);
  LocalFree(lpMsgBuf);
}

static void
InitializeDbgHelpCriticalSection()
{
  static bool initialized = false;
  if (initialized) {
    return;
  }
  ::InitializeCriticalSection(&gDbgHelpCS);
  initialized = true;
}

static unsigned int WINAPI WalkStackThread(void* aData);

static bool
EnsureWalkThreadReady()
{
  static bool walkThreadReady = false;
  static HANDLE stackWalkThread = nullptr;
  static HANDLE readyEvent = nullptr;

  if (walkThreadReady) {
    return walkThreadReady;
  }

  if (!stackWalkThread) {
    readyEvent = ::CreateEvent(nullptr, FALSE /* auto-reset*/,
                               FALSE /* initially non-signaled */,
                               nullptr);
    if (!readyEvent) {
      PrintError("CreateEvent");
      return false;
    }

    unsigned int threadID;
    stackWalkThread = (HANDLE)_beginthreadex(nullptr, 0, WalkStackThread,
                                             (void*)readyEvent, 0, &threadID);
    if (!stackWalkThread) {
      PrintError("CreateThread");
      ::CloseHandle(readyEvent);
      readyEvent = nullptr;
      return false;
    }
    gStackWalkThread = threadID;
    ::CloseHandle(stackWalkThread);
  }

  MOZ_ASSERT((stackWalkThread && readyEvent) ||
             (!stackWalkThread && !readyEvent));

  // The thread was created. Try to wait an arbitrary amount of time (1 second
  // should be enough) for its event loop to start before posting events to it.
  DWORD waitRet = ::WaitForSingleObject(readyEvent, 1000);
  if (waitRet == WAIT_TIMEOUT) {
    // We get a timeout if we're called during static initialization because
    // the thread will only start executing after we return so it couldn't
    // have signalled the event. If that is the case, give up for now and
    // try again next time we're called.
    return false;
  }
  ::CloseHandle(readyEvent);
  stackWalkThread = nullptr;
  readyEvent = nullptr;

  return walkThreadReady = true;
}

static void
WalkStackMain64(struct WalkStackData* aData)
{
  // Get a context for the specified thread.
  CONTEXT context_buf;
  CONTEXT* context;
  if (!aData->context) {
    context = &context_buf;
    memset(context, 0, sizeof(CONTEXT));
    context->ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(aData->thread, context)) {
      if (aData->walkCallingThread) {
        PrintError("GetThreadContext");
      }
      return;
    }
  } else {
    context = aData->context;
  }

#if defined(_M_IX86) || defined(_M_IA64)
  // Setup initial stack frame to walk from.
  STACKFRAME64 frame64;
  memset(&frame64, 0, sizeof(frame64));
#ifdef _M_IX86
  frame64.AddrPC.Offset    = context->Eip;
  frame64.AddrStack.Offset = context->Esp;
  frame64.AddrFrame.Offset = context->Ebp;
#elif defined _M_IA64
  frame64.AddrPC.Offset    = context->StIIP;
  frame64.AddrStack.Offset = context->SP;
  frame64.AddrFrame.Offset = context->RsBSP;
#endif
  frame64.AddrPC.Mode      = AddrModeFlat;
  frame64.AddrStack.Mode   = AddrModeFlat;
  frame64.AddrFrame.Mode   = AddrModeFlat;
  frame64.AddrReturn.Mode  = AddrModeFlat;
#endif

#ifdef _WIN64
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
#endif

#ifdef _M_AMD64
  bool firstFrame = true;
#endif

  // Skip our own stack walking frames.
  int skip = (aData->walkCallingThread ? 3 : 0) + aData->skipFrames;

  // Now walk the stack.
  while (true) {
    DWORD64 addr;
    DWORD64 spaddr;

#if defined(_M_IX86) || defined(_M_IA64)
    // 32-bit frame unwinding.
    // Debug routines are not threadsafe, so grab the lock.
    EnterCriticalSection(&gDbgHelpCS);
    BOOL ok = StackWalk64(
#if defined _M_IA64
      IMAGE_FILE_MACHINE_IA64,
#elif defined _M_IX86
      IMAGE_FILE_MACHINE_I386,
#endif
      aData->process,
      aData->thread,
      &frame64,
      context,
      nullptr,
      SymFunctionTableAccess64, // function table access routine
      SymGetModuleBase64,       // module base routine
      0
    );
    LeaveCriticalSection(&gDbgHelpCS);

    if (ok) {
      addr = frame64.AddrPC.Offset;
      spaddr = frame64.AddrStack.Offset;
    } else {
      addr = 0;
      spaddr = 0;
      if (aData->walkCallingThread) {
        PrintError("WalkStack64");
      }
    }

    if (!ok) {
      break;
    }

#elif defined(_M_AMD64)
    // If we reach a frame in JIT code, we don't have enough information to
    // unwind, so we have to give up.
    if (sJitCodeRegionStart &&
        (uint8_t*)context->Rip >= sJitCodeRegionStart &&
        (uint8_t*)context->Rip < sJitCodeRegionStart + sJitCodeRegionSize) {
      break;
    }

    // We must also avoid msmpeg2vdec.dll's JIT region: they don't generate
    // unwind data, so their JIT unwind callback just throws up its hands and
    // terminates the process.
    if (sMsMpegJitCodeRegionStart &&
        (uint8_t*)context->Rip >= sMsMpegJitCodeRegionStart &&
        (uint8_t*)context->Rip < sMsMpegJitCodeRegionStart + sMsMpegJitCodeRegionSize) {
      break;
    }

    // 64-bit frame unwinding.
    // Try to look up unwind metadata for the current function.
    ULONG64 imageBase;
    PRUNTIME_FUNCTION runtimeFunction =
      RtlLookupFunctionEntry(context->Rip, &imageBase, NULL);

    if (runtimeFunction) {
      PVOID dummyHandlerData;
      ULONG64 dummyEstablisherFrame;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                       imageBase,
                       context->Rip,
                       runtimeFunction,
                       context,
                       &dummyHandlerData,
                       &dummyEstablisherFrame,
                       nullptr);
    } else if (firstFrame) {
      // Leaf functions can be unwound by hand.
      context->Rip = *reinterpret_cast<DWORD64*>(context->Rsp);
      context->Rsp += sizeof(void*);
    } else {
      // Something went wrong.
      break;
    }

    addr = context->Rip;
    spaddr = context->Rsp;
    firstFrame = false;
#else
#error "unknown platform"
#endif

    if (addr == 0) {
      break;
    }

    if (skip-- > 0) {
      continue;
    }

    if (aData->pc_count < aData->pc_size) {
      aData->pcs[aData->pc_count] = (void*)addr;
    }
    ++aData->pc_count;

    if (aData->sp_count < aData->sp_size) {
      aData->sps[aData->sp_count] = (void*)spaddr;
    }
    ++aData->sp_count;

    if (aData->pc_max != 0 && aData->pc_count == aData->pc_max) {
      break;
    }

#if defined(_M_IX86) || defined(_M_IA64)
    if (frame64.AddrReturn.Offset == 0) {
      break;
    }
#endif
  }
}

static unsigned int WINAPI
WalkStackThread(void* aData)
{
  BOOL msgRet;
  MSG msg;

  // Call PeekMessage to force creation of a message queue so that
  // other threads can safely post events to us.
  ::PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

  // and tell the thread that created us that we're ready.
  HANDLE readyEvent = (HANDLE)aData;
  ::SetEvent(readyEvent);

  while ((msgRet = ::GetMessage(&msg, (HWND)-1, 0, 0)) != 0) {
    if (msgRet == -1) {
      PrintError("GetMessage");
    } else {
      DWORD ret;

      struct WalkStackData* data = (WalkStackData*)msg.lParam;
      if (!data) {
        continue;
      }

      // Don't suspend the calling thread until it's waiting for
      // us; otherwise the number of frames on the stack could vary.
      ret = ::WaitForSingleObject(data->eventStart, INFINITE);
      if (ret != WAIT_OBJECT_0) {
        PrintError("WaitForSingleObject");
      }

      // Suspend the calling thread, dump his stack, and then resume him.
      // He's currently waiting for us to finish so now should be a good time.
      ret = ::SuspendThread(data->thread);
      if (ret == (DWORD)-1) {
        PrintError("ThreadSuspend");
      } else {
        WalkStackMain64(data);

        ret = ::ResumeThread(data->thread);
        if (ret == (DWORD)-1) {
          PrintError("ThreadResume");
        }
      }

      ::SetEvent(data->eventEnd);
    }
  }

  return 0;
}

/**
 * Walk the stack, translating PC's found into strings and recording the
 * chain in aBuffer. For this to work properly, the DLLs must be rebased
 * so that the address in the file agrees with the address in memory.
 * Otherwise StackWalk will return FALSE when it hits a frame in a DLL
 * whose in memory address doesn't match its in-file address.
 */

MFBT_API void
MozStackWalkThread(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
                   uint32_t aMaxFrames, void* aClosure,
                   HANDLE aThread, CONTEXT* aContext)
{
  static HANDLE myProcess = nullptr;
  HANDLE myThread;
  DWORD walkerReturn;
  struct WalkStackData data;

  InitializeDbgHelpCriticalSection();

  // EnsureWalkThreadReady's _beginthreadex takes a heap lock and must be
  // avoided if we're walking another (i.e. suspended) thread.
  if (!aThread && !EnsureWalkThreadReady()) {
    return;
  }

  HANDLE currentThread = ::GetCurrentThread();
  HANDLE targetThread = aThread ? aThread : currentThread;
  data.walkCallingThread = (targetThread == currentThread);

  // Have to duplicate handle to get a real handle.
  if (!myProcess) {
    if (!::DuplicateHandle(::GetCurrentProcess(),
                           ::GetCurrentProcess(),
                           ::GetCurrentProcess(),
                           &myProcess,
                           PROCESS_ALL_ACCESS, FALSE, 0)) {
      if (data.walkCallingThread) {
        PrintError("DuplicateHandle (process)");
      }
      return;
    }
  }
  if (!::DuplicateHandle(::GetCurrentProcess(),
                         targetThread,
                         ::GetCurrentProcess(),
                         &myThread,
                         THREAD_ALL_ACCESS, FALSE, 0)) {
    if (data.walkCallingThread) {
      PrintError("DuplicateHandle (thread)");
    }
    return;
  }

  data.skipFrames = aSkipFrames;
  data.thread = myThread;
  data.process = myProcess;
  void* local_pcs[1024];
  data.pcs = local_pcs;
  data.pc_count = 0;
  data.pc_size = ArrayLength(local_pcs);
  data.pc_max = aMaxFrames;
  void* local_sps[1024];
  data.sps = local_sps;
  data.sp_count = 0;
  data.sp_size = ArrayLength(local_sps);
  data.context = aContext;

  if (aThread) {
    // If we're walking the stack of another thread, we don't need to
    // use a separate walker thread.
    WalkStackMain64(&data);

    if (data.pc_count > data.pc_size) {
      data.pcs = (void**)_alloca(data.pc_count * sizeof(void*));
      data.pc_size = data.pc_count;
      data.pc_count = 0;
      data.sps = (void**)_alloca(data.sp_count * sizeof(void*));
      data.sp_size = data.sp_count;
      data.sp_count = 0;
      WalkStackMain64(&data);
    }
  } else {
    data.eventStart = ::CreateEvent(nullptr, FALSE /* auto-reset*/,
                                    FALSE /* initially non-signaled */, nullptr);
    data.eventEnd = ::CreateEvent(nullptr, FALSE /* auto-reset*/,
                                  FALSE /* initially non-signaled */, nullptr);

    ::PostThreadMessage(gStackWalkThread, WM_USER, 0, (LPARAM)&data);

    walkerReturn = ::SignalObjectAndWait(data.eventStart,
                                         data.eventEnd, INFINITE, FALSE);
    if (walkerReturn != WAIT_OBJECT_0 && data.walkCallingThread) {
      PrintError("SignalObjectAndWait (1)");
    }
    if (data.pc_count > data.pc_size) {
      data.pcs = (void**)_alloca(data.pc_count * sizeof(void*));
      data.pc_size = data.pc_count;
      data.pc_count = 0;
      data.sps = (void**)_alloca(data.sp_count * sizeof(void*));
      data.sp_size = data.sp_count;
      data.sp_count = 0;
      ::PostThreadMessage(gStackWalkThread, WM_USER, 0, (LPARAM)&data);
      walkerReturn = ::SignalObjectAndWait(data.eventStart,
                                           data.eventEnd, INFINITE, FALSE);
      if (walkerReturn != WAIT_OBJECT_0 && data.walkCallingThread) {
        PrintError("SignalObjectAndWait (2)");
      }
    }

    ::CloseHandle(data.eventStart);
    ::CloseHandle(data.eventEnd);
  }

  ::CloseHandle(myThread);

  for (uint32_t i = 0; i < data.pc_count; ++i) {
    (*aCallback)(i + 1, data.pcs[i], data.sps[i], aClosure);
  }
}

MFBT_API void
MozStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void* aClosure)
{
  MozStackWalkThread(aCallback, aSkipFrames, aMaxFrames, aClosure,
                     nullptr, nullptr);
}

static BOOL CALLBACK
callbackEspecial64(
  PCSTR aModuleName,
  DWORD64 aModuleBase,
  ULONG aModuleSize,
  PVOID aUserContext)
{
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
      : (addr <= aModuleBase && addr >= (aModuleBase - aModuleSize))
     ) {
    retval = !!SymLoadModule64(GetCurrentProcess(), nullptr,
                               (PSTR)aModuleName, nullptr,
                               aModuleBase, aModuleSize);
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
#ifdef SSRVOPT_SETCONTEXT
#define NS_IMAGEHLP_MODULE64_SIZE (((offsetof(IMAGEHLP_MODULE64, LoadedPdbName) + sizeof(DWORD64) - 1) / sizeof(DWORD64)) * sizeof(DWORD64))
#else
#define NS_IMAGEHLP_MODULE64_SIZE sizeof(IMAGEHLP_MODULE64)
#endif

BOOL SymGetModuleInfoEspecial64(HANDLE aProcess, DWORD64 aAddr,
                                PIMAGEHLP_MODULE64 aModuleInfo,
                                PIMAGEHLP_LINE64 aLineInfo)
{
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
      aProcess,
      (PENUMLOADED_MODULES_CALLBACK64)callbackEspecial64,
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

static bool
EnsureSymInitialized()
{
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


MFBT_API bool
MozDescribeCodeAddress(void* aPC, MozCodeAddressDetails* aDetails)
{
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

  ULONG64 buffer[(sizeof(SYMBOL_INFO) +
    MAX_SYM_NAME * sizeof(TCHAR) + sizeof(ULONG64) - 1) / sizeof(ULONG64)];
  PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = MAX_SYM_NAME;

  DWORD64 displacement;
  ok = SymFromAddr(myProcess, addr, &displacement, pSymbol);

  if (ok) {
    strncpy(aDetails->function, pSymbol->Name,
                sizeof(aDetails->function));
    aDetails->function[mozilla::ArrayLength(aDetails->function) - 1] = '\0';
    aDetails->foffset = static_cast<ptrdiff_t>(displacement);
  }

  LeaveCriticalSection(&gDbgHelpCS); // release our lock
  return true;
}

// i386 or PPC Linux stackwalking code
#elif HAVE_DLADDR && (HAVE__UNWIND_BACKTRACE || MOZ_STACKWALK_SUPPORTS_LINUX || MOZ_STACKWALK_SUPPORTS_MACOSX)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// On glibc 2.1, the Dl_info api defined in <dlfcn.h> is only exposed
// if __USE_GNU is defined.  I suppose its some kind of standards
// adherence thing.
//
#if (__GLIBC_MINOR__ >= 1) && !defined(__USE_GNU)
#define __USE_GNU
#endif

// This thing is exported by libstdc++
// Yes, this is a gcc only hack
#if defined(MOZ_DEMANGLE_SYMBOLS)
#include <cxxabi.h>
#endif // MOZ_DEMANGLE_SYMBOLS

void DemangleSymbol(const char* aSymbol,
                    char* aBuffer,
                    int aBufLen)
{
  aBuffer[0] = '\0';

#if defined(MOZ_DEMANGLE_SYMBOLS)
  /* See demangle.h in the gcc source for the voodoo */
  char* demangled = abi::__cxa_demangle(aSymbol, 0, 0, 0);

  if (demangled) {
    strncpy(aBuffer, demangled, aBufLen);
    aBuffer[aBufLen - 1] = '\0';
    free(demangled);
  }
#endif // MOZ_DEMANGLE_SYMBOLS
}

// {x86, ppc} x {Linux, Mac} stackwalking code.
#if ((defined(__i386) || defined(PPC) || defined(__ppc__)) && \
     (MOZ_STACKWALK_SUPPORTS_MACOSX || MOZ_STACKWALK_SUPPORTS_LINUX))

MFBT_API void
MozStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void* aClosure)
{
  // Get the frame pointer
  void** bp = (void**)__builtin_frame_address(0);

  void* stackEnd;
#if HAVE___LIBC_STACK_END
  stackEnd = __libc_stack_end;
#elif defined(XP_DARWIN)
  stackEnd = pthread_get_stackaddr_np(pthread_self());
#elif defined(ANDROID)
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
#else
#  error Unsupported configuration
#endif
  FramePointerStackWalk(aCallback, aSkipFrames, aMaxFrames, aClosure, bp,
                        stackEnd);
}

#elif defined(HAVE__UNWIND_BACKTRACE)

// libgcc_s.so symbols _Unwind_Backtrace@@GCC_3.3 and _Unwind_GetIP@@GCC_3.0
#include <unwind.h>

struct unwind_info
{
  MozWalkStackCallback callback;
  int skip;
  int maxFrames;
  int numFrames;
  void* closure;
};

static _Unwind_Reason_Code
unwind_callback(struct _Unwind_Context* context, void* closure)
{
  unwind_info* info = static_cast<unwind_info*>(closure);
  void* pc = reinterpret_cast<void*>(_Unwind_GetIP(context));
  // TODO Use something like '_Unwind_GetGR()' to get the stack pointer.
  if (--info->skip < 0) {
    info->numFrames++;
    (*info->callback)(info->numFrames, pc, nullptr, info->closure);
    if (info->maxFrames != 0 && info->numFrames == info->maxFrames) {
      // Again, any error code that stops the walk will do.
      return _URC_FOREIGN_EXCEPTION_CAUGHT;
    }
  }
  return _URC_NO_REASON;
}

MFBT_API void
MozStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void* aClosure)
{
  unwind_info info;
  info.callback = aCallback;
  info.skip = aSkipFrames + 1;
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

#endif

bool MFBT_API
MozDescribeCodeAddress(void* aPC, MozCodeAddressDetails* aDetails)
{
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  Dl_info info;
  int ok = dladdr(aPC, &info);
  if (!ok) {
    return true;
  }

  strncpy(aDetails->library, info.dli_fname, sizeof(aDetails->library));
  aDetails->library[mozilla::ArrayLength(aDetails->library) - 1] = '\0';
  aDetails->loffset = (char*)aPC - (char*)info.dli_fbase;

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
  return true;
}

#else // unsupported platform.

MFBT_API void
MozStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void* aClosure)
{
}

MFBT_API bool
MozDescribeCodeAddress(void* aPC, MozCodeAddressDetails* aDetails)
{
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;
  return false;
}

#endif

#if defined(XP_WIN) || defined (XP_MACOSX) || defined (XP_LINUX)
namespace mozilla {
void
FramePointerStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
                      uint32_t aMaxFrames, void* aClosure, void** aBp,
                      void* aStackEnd)
{
  // Stack walking code courtesy Kipp's "leaky".

  int32_t skip = aSkipFrames;
  uint32_t numFrames = 0;
  while (aBp) {
    void** next = (void**)*aBp;
    // aBp may not be a frame pointer on i386 if code was compiled with
    // -fomit-frame-pointer, so do some sanity checks.
    // (aBp should be a frame pointer on ppc(64) but checking anyway may help
    // a little if the stack has been corrupted.)
    // We don't need to check against the begining of the stack because
    // we can assume that aBp > sp
    if (next <= aBp ||
        next > aStackEnd ||
        (uintptr_t(next) & 3)) {
      break;
    }
#if (defined(__ppc__) && defined(XP_MACOSX)) || defined(__powerpc64__)
    // ppc mac or powerpc64 linux
    void* pc = *(aBp + 2);
    aBp += 3;
#else // i386 or powerpc32 linux
    void* pc = *(aBp + 1);
    aBp += 2;
#endif
    if (--skip < 0) {
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
} // namespace mozilla

#else

namespace mozilla {
MFBT_API void
FramePointerStackWalk(MozWalkStackCallback aCallback, uint32_t aSkipFrames,
                      uint32_t aMaxFrames, void* aClosure, void** aBp,
                      void* aStackEnd)
{
}
}

#endif

MFBT_API void
MozFormatCodeAddressDetails(char* aBuffer, uint32_t aBufferSize,
                            uint32_t aFrameNumber, void* aPC,
                            const MozCodeAddressDetails* aDetails)
{
  MozFormatCodeAddress(aBuffer, aBufferSize,
                       aFrameNumber, aPC, aDetails->function,
                       aDetails->library, aDetails->loffset,
                       aDetails->filename, aDetails->lineno);
}

MFBT_API void
MozFormatCodeAddress(char* aBuffer, uint32_t aBufferSize, uint32_t aFrameNumber,
                     const void* aPC, const char* aFunction,
                     const char* aLibrary, ptrdiff_t aLOffset,
                     const char* aFileName, uint32_t aLineNo)
{
  const char* function = aFunction && aFunction[0] ? aFunction : "???";
  if (aFileName && aFileName[0]) {
    // We have a filename and (presumably) a line number. Use them.
    snprintf(aBuffer, aBufferSize,
             "#%02u: %s (%s:%u)",
             aFrameNumber, function, aFileName, aLineNo);
  } else if (aLibrary && aLibrary[0]) {
    // We have no filename, but we do have a library name. Use it and the
    // library offset, and print them in a way that scripts like
    // fix_{linux,macosx}_stacks.py can easily post-process.
    snprintf(aBuffer, aBufferSize,
             "#%02u: %s[%s +0x%" PRIxPTR "]",
             aFrameNumber, function, aLibrary, static_cast<uintptr_t>(aLOffset));
  } else {
    // We have nothing useful to go on. (The format string is split because
    // '??)' is a trigraph and causes a warning, sigh.)
    snprintf(aBuffer, aBufferSize,
             "#%02u: ??? (???:???" ")",
             aFrameNumber);
  }
}
