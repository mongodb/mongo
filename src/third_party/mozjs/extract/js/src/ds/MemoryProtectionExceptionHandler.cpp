/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ds/MemoryProtectionExceptionHandler.h"

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"

#if defined(XP_WIN)
#  include "util/Windows.h"
#elif defined(__wasi__)
// Nothing.
#elif defined(XP_UNIX) && !defined(XP_DARWIN)
#  include <signal.h>
#  include <sys/types.h>
#  include <unistd.h>
#elif defined(XP_DARWIN)
#  include <mach/mach.h>
#  include <unistd.h>
#endif

#ifdef ANDROID
#  include <android/log.h>
#endif

#include "ds/SplayTree.h"

#include "threading/LockGuard.h"
#include "threading/Thread.h"
#include "vm/MutexIDs.h"
#include "vm/Runtime.h"

namespace js {

// Memory protection occurs at non-deterministic points when
// recording/replaying.
static mozilla::Atomic<bool, mozilla::SequentiallyConsistent>
    sProtectedRegionsInit(false);

/*
 * A class to store the addresses of the regions recognized as protected
 * by this exception handler. We use a splay tree to store these addresses.
 */
class ProtectedRegionTree {
  struct Region {
    uintptr_t first;
    uintptr_t last;

    Region(uintptr_t addr, size_t size)
        : first(addr), last(addr + (size - 1)) {}

    // This function compares 2 memory regions. If they overlap they are
    // considered as identical. This is used for querying if an address is
    // included in a range, or if an address is already registered as a
    // protected region.
    static int compare(const Region& A, const Region& B) {
      if (A.last < B.first) {
        return -1;
      }
      if (A.first > B.last) {
        return 1;
      }
      return 0;
    }
  };

  Mutex lock;
  LifoAlloc alloc;
  SplayTree<Region, Region> tree;

 public:
  ProtectedRegionTree()
      : lock(mutexid::ProtectedRegionTree),
        // Here "false" is used to not use the memory protection mechanism of
        // LifoAlloc in order to prevent dead-locks.
        alloc(4096),
        tree(&alloc) {
    sProtectedRegionsInit = true;
  }

  ~ProtectedRegionTree() { sProtectedRegionsInit = false; }

  void insert(uintptr_t addr, size_t size) {
    MOZ_ASSERT(addr && size);
    LockGuard<Mutex> guard(lock);
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!tree.insert(Region(addr, size))) {
      oomUnsafe.crash("Failed to store allocation ID.");
    }
  }

  void remove(uintptr_t addr) {
    MOZ_ASSERT(addr);
    LockGuard<Mutex> guard(lock);
    tree.remove(Region(addr, 1));
  }

  bool isProtected(uintptr_t addr) {
    if (!addr) {
      return false;
    }
    LockGuard<Mutex> guard(lock);
    return tree.maybeLookup(Region(addr, 1));
  }
};

static bool sExceptionHandlerInstalled = false;

static ProtectedRegionTree sProtectedRegions;

bool MemoryProtectionExceptionHandler::isDisabled() {
#if defined(XP_WIN) && defined(MOZ_ASAN)
  // Under Windows ASan, WasmFaultHandler registers itself at 'last' priority
  // in order to let ASan's ShadowExceptionHandler stay at 'first' priority.
  // Unfortunately that results in spurious wasm faults passing through the
  // MemoryProtectionExceptionHandler, which breaks its assumption that any
  // faults it sees are fatal. Just disable this handler in that case, as the
  // crash annotations provided here are not critical for ASan builds.
  return true;
#elif !defined(MOZ_DIAGNOSTIC_ASSERT_ENABLED)
  // Disable the exception handler for Beta and Release builds.
  return true;
#elif defined(__wasi__)
  return true;
#else
  return false;
#endif
}

void MemoryProtectionExceptionHandler::addRegion(void* addr, size_t size) {
  if (sExceptionHandlerInstalled && sProtectedRegionsInit) {
    sProtectedRegions.insert(uintptr_t(addr), size);
  }
}

void MemoryProtectionExceptionHandler::removeRegion(void* addr) {
  if (sExceptionHandlerInstalled && sProtectedRegionsInit) {
    sProtectedRegions.remove(uintptr_t(addr));
  }
}

/* -------------------------------------------------------------------------- */

/*
 * This helper function attempts to replicate the functionality of
 * mozilla::MOZ_ReportCrash() in an async-signal-safe way.
 */
static MOZ_COLD MOZ_ALWAYS_INLINE void ReportCrashIfDebug(const char* aStr)
    MOZ_PRETEND_NORETURN_FOR_STATIC_ANALYSIS {
#ifdef DEBUG
#  if defined(XP_WIN)
  DWORD bytesWritten;
  BOOL ret = WriteFile(GetStdHandle(STD_ERROR_HANDLE), aStr, strlen(aStr) + 1,
                       &bytesWritten, nullptr);
#  elif defined(ANDROID)
  int ret = __android_log_write(ANDROID_LOG_FATAL, "MOZ_CRASH", aStr);
#  else
  ssize_t ret = write(STDERR_FILENO, aStr, strlen(aStr) + 1);
#  endif
  (void)ret;  // Ignore failures; we're already crashing anyway.
#endif
}

/* -------------------------------------------------------------------------- */

#if defined(XP_WIN)

static void* sVectoredExceptionHandler = nullptr;

/*
 * We can only handle one exception. To guard against races and reentrancy,
 * we set this value the first time we enter the exception handler and never
 * touch it again.
 */
static mozilla::Atomic<bool> sHandlingException(false);

static long __stdcall VectoredExceptionHandler(
    EXCEPTION_POINTERS* ExceptionInfo) {
  EXCEPTION_RECORD* ExceptionRecord = ExceptionInfo->ExceptionRecord;

  // We only handle one kind of exception; ignore all others.
  if (ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    // Make absolutely sure we can only get here once.
    if (sHandlingException.compareExchange(false, true)) {
      // Restore the previous handler. We're going to forward to it
      // anyway, and if we crash while doing so we don't want to hang.
      MOZ_ALWAYS_TRUE(
          RemoveVectoredExceptionHandler(sVectoredExceptionHandler));
      sExceptionHandlerInstalled = false;

      // Get the address that the offending code tried to access.
      uintptr_t address = uintptr_t(ExceptionRecord->ExceptionInformation[1]);

      // If the faulting address is in one of our protected regions, we
      // want to annotate the crash to make it stand out from the crowd.
      if (sProtectedRegionsInit && sProtectedRegions.isProtected(address)) {
        ReportCrashIfDebug(
            "Hit MOZ_CRASH(Tried to access a protected region!)\n");
        MOZ_CRASH_ANNOTATE("MOZ_CRASH(Tried to access a protected region!)");
      }
    }
  }

  // Forward to the previous handler which may be a debugger,
  // the crash reporter or something else entirely.
  return EXCEPTION_CONTINUE_SEARCH;
}

bool MemoryProtectionExceptionHandler::install() {
  MOZ_ASSERT(!sExceptionHandlerInstalled);

  // If the exception handler is disabled, report success anyway.
  if (MemoryProtectionExceptionHandler::isDisabled()) {
    return true;
  }

  // Install our new exception handler.
  sVectoredExceptionHandler = AddVectoredExceptionHandler(
      /* FirstHandler = */ true, VectoredExceptionHandler);

  sExceptionHandlerInstalled = sVectoredExceptionHandler != nullptr;
  return sExceptionHandlerInstalled;
}

void MemoryProtectionExceptionHandler::uninstall() {
  if (sExceptionHandlerInstalled) {
    MOZ_ASSERT(!sHandlingException);

    // Restore the previous exception handler.
    MOZ_ALWAYS_TRUE(RemoveVectoredExceptionHandler(sVectoredExceptionHandler));

    sExceptionHandlerInstalled = false;
  }
}

#elif defined(__wasi__)

bool MemoryProtectionExceptionHandler::install() { return true; }

void MemoryProtectionExceptionHandler::uninstall() {}

#elif defined(XP_UNIX) && !defined(XP_DARWIN)

static struct sigaction sPrevSEGVHandler = {};

/*
 * We can only handle one exception. To guard against races and reentrancy,
 * we set this value the first time we enter the exception handler and never
 * touch it again.
 */
static mozilla::Atomic<bool> sHandlingException(false);

static void UnixExceptionHandler(int signum, siginfo_t* info, void* context) {
  // Make absolutely sure we can only get here once.
  if (sHandlingException.compareExchange(false, true)) {
    // Restore the previous handler. We're going to forward to it
    // anyway, and if we crash while doing so we don't want to hang.
    MOZ_ALWAYS_FALSE(sigaction(SIGSEGV, &sPrevSEGVHandler, nullptr));

    MOZ_ASSERT(signum == SIGSEGV && info->si_signo == SIGSEGV);

    if (info->si_code == SEGV_ACCERR) {
      // Get the address that the offending code tried to access.
      uintptr_t address = uintptr_t(info->si_addr);

      // If the faulting address is in one of our protected regions, we
      // want to annotate the crash to make it stand out from the crowd.
      if (sProtectedRegionsInit && sProtectedRegions.isProtected(address)) {
        ReportCrashIfDebug(
            "Hit MOZ_CRASH(Tried to access a protected region!)\n");
        MOZ_CRASH_ANNOTATE("MOZ_CRASH(Tried to access a protected region!)");
      }
    }
  }

  // Forward to the previous handler which may be a debugger,
  // the crash reporter or something else entirely.
  if (sPrevSEGVHandler.sa_flags & SA_SIGINFO) {
    sPrevSEGVHandler.sa_sigaction(signum, info, context);
  } else if (sPrevSEGVHandler.sa_handler == SIG_DFL ||
             sPrevSEGVHandler.sa_handler == SIG_IGN) {
    sigaction(SIGSEGV, &sPrevSEGVHandler, nullptr);
  } else {
    sPrevSEGVHandler.sa_handler(signum);
  }

  // If we reach here, we're returning to let the default signal handler deal
  // with the exception. This is technically undefined behavior, but
  // everything seems to do it, and it removes us from the crash stack.
}

bool MemoryProtectionExceptionHandler::install() {
  MOZ_ASSERT(!sExceptionHandlerInstalled);

  // If the exception handler is disabled, report success anyway.
  if (MemoryProtectionExceptionHandler::isDisabled()) {
    return true;
  }

  // Install our new exception handler and save the previous one.
  struct sigaction faultHandler = {};
  faultHandler.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  faultHandler.sa_sigaction = UnixExceptionHandler;
  sigemptyset(&faultHandler.sa_mask);
  sExceptionHandlerInstalled =
      !sigaction(SIGSEGV, &faultHandler, &sPrevSEGVHandler);

  return sExceptionHandlerInstalled;
}

void MemoryProtectionExceptionHandler::uninstall() {
  if (sExceptionHandlerInstalled) {
    MOZ_ASSERT(!sHandlingException);

    // Restore the previous exception handler.
    MOZ_ALWAYS_FALSE(sigaction(SIGSEGV, &sPrevSEGVHandler, nullptr));

    sExceptionHandlerInstalled = false;
  }
}

#elif defined(XP_DARWIN)

/*
 * The fact that we need to be able to forward to other exception handlers
 * makes this code much more complicated. The forwarding logic and the
 * structures required to make it work are heavily based on the code at
 * www.ravenbrook.com/project/mps/prototype/2013-06-24/machtest/machtest/main.c
 */

/* -------------------------------------------------------------------------- */
/*                Begin Mach definitions and helper functions                 */
/* -------------------------------------------------------------------------- */

/*
 * These are the message IDs associated with each exception type.
 * We'll be using sIDRequest64, but we need the others for forwarding.
 */
static const mach_msg_id_t sIDRequest32 = 2401;
static const mach_msg_id_t sIDRequestState32 = 2402;
static const mach_msg_id_t sIDRequestStateIdentity32 = 2403;

static const mach_msg_id_t sIDRequest64 = 2405;
static const mach_msg_id_t sIDRequestState64 = 2406;
static const mach_msg_id_t sIDRequestStateIdentity64 = 2407;

/*
 * Each message ID has an associated Mach message structure.
 * We use the preprocessor to make defining them a little less arduous.
 */
#  define REQUEST_HEADER_FIELDS mach_msg_header_t header;

#  define REQUEST_IDENTITY_FIELDS      \
    mach_msg_body_t msgh_body;         \
    mach_msg_port_descriptor_t thread; \
    mach_msg_port_descriptor_t task;

#  define REQUEST_GENERAL_FIELDS(bits) \
    NDR_record_t NDR;                  \
    exception_type_t exception;        \
    mach_msg_type_number_t code_count; \
    int##bits##_t code[2];

#  define REQUEST_STATE_FIELDS              \
    int flavor;                             \
    mach_msg_type_number_t old_state_count; \
    natural_t old_state[THREAD_STATE_MAX];

#  define REQUEST_TRAILER_FIELDS mach_msg_trailer_t trailer;

#  define EXCEPTION_REQUEST(bits)   \
    struct ExceptionRequest##bits { \
      REQUEST_HEADER_FIELDS         \
      REQUEST_IDENTITY_FIELDS       \
      REQUEST_GENERAL_FIELDS(bits)  \
      REQUEST_TRAILER_FIELDS        \
    };

#  define EXCEPTION_REQUEST_STATE(bits)  \
    struct ExceptionRequestState##bits { \
      REQUEST_HEADER_FIELDS              \
      REQUEST_GENERAL_FIELDS(bits)       \
      REQUEST_STATE_FIELDS               \
      REQUEST_TRAILER_FIELDS             \
    };

#  define EXCEPTION_REQUEST_STATE_IDENTITY(bits) \
    struct ExceptionRequestStateIdentity##bits { \
      REQUEST_HEADER_FIELDS                      \
      REQUEST_IDENTITY_FIELDS                    \
      REQUEST_GENERAL_FIELDS(bits)               \
      REQUEST_STATE_FIELDS                       \
      REQUEST_TRAILER_FIELDS                     \
    };

/* This is needed because not all fields are naturally aligned on 64-bit. */
#  ifdef __MigPackStructs
#    pragma pack(4)
#  endif

EXCEPTION_REQUEST(32)
EXCEPTION_REQUEST(64)
EXCEPTION_REQUEST_STATE(32)
EXCEPTION_REQUEST_STATE(64)
EXCEPTION_REQUEST_STATE_IDENTITY(32)
EXCEPTION_REQUEST_STATE_IDENTITY(64)

/* We use this as a common base when forwarding to the previous handler. */
union ExceptionRequestUnion {
  mach_msg_header_t header;
  ExceptionRequest32 r32;
  ExceptionRequest64 r64;
  ExceptionRequestState32 rs32;
  ExceptionRequestState64 rs64;
  ExceptionRequestStateIdentity32 rsi32;
  ExceptionRequestStateIdentity64 rsi64;
};

/* This isn't really a full Mach message, but it's all we need to send. */
struct ExceptionReply {
  mach_msg_header_t header;
  NDR_record_t NDR;
  kern_return_t RetCode;
};

#  ifdef __MigPackStructs
#    pragma pack()
#  endif

#  undef EXCEPTION_REQUEST_STATE_IDENTITY
#  undef EXCEPTION_REQUEST_STATE
#  undef EXCEPTION_REQUEST
#  undef REQUEST_STATE_FIELDS
#  undef REQUEST_GENERAL_FIELDS
#  undef REQUEST_IDENTITY_FIELDS
#  undef REQUEST_HEADER_FIELDS

/*
 * The exception handler we're forwarding to may not have the same behavior
 * or thread state flavor as what we're using. These macros help populate
 * the fields of the message we're about to send to the previous handler.
 */
#  define COPY_REQUEST_COMMON(bits, id)                                  \
    dst.header = src.header;                                             \
    dst.header.msgh_id = id;                                             \
    dst.header.msgh_size =                                               \
        static_cast<mach_msg_size_t>(sizeof(dst) - sizeof(dst.trailer)); \
    dst.NDR = src.NDR;                                                   \
    dst.exception = src.exception;                                       \
    dst.code_count = src.code_count;                                     \
    dst.code[0] = int##bits##_t(src.code[0]);                            \
    dst.code[1] = int##bits##_t(src.code[1]);

#  define COPY_REQUEST_IDENTITY    \
    dst.msgh_body = src.msgh_body; \
    dst.thread = src.thread;       \
    dst.task = src.task;

#  define COPY_REQUEST_STATE(flavor, stateCount, state)                  \
    mach_msg_size_t stateSize = stateCount * sizeof(natural_t);          \
    dst.header.msgh_size =                                               \
        static_cast<mach_msg_size_t>(sizeof(dst) - sizeof(dst.trailer) - \
                                     sizeof(dst.old_state) + stateSize); \
    dst.flavor = flavor;                                                 \
    dst.old_state_count = stateCount;                                    \
    memcpy(dst.old_state, state, stateSize);

#  define COPY_EXCEPTION_REQUEST(bits)                                    \
    static void CopyExceptionRequest##bits(ExceptionRequest64& src,       \
                                           ExceptionRequest##bits& dst) { \
      COPY_REQUEST_COMMON(bits, sIDRequest##bits)                         \
      COPY_REQUEST_IDENTITY                                               \
    }

#  define COPY_EXCEPTION_REQUEST_STATE(bits)                             \
    static void CopyExceptionRequestState##bits(                         \
        ExceptionRequest64& src, ExceptionRequestState##bits& dst,       \
        thread_state_flavor_t flavor, mach_msg_type_number_t stateCount, \
        thread_state_t state) {                                          \
      COPY_REQUEST_COMMON(bits, sIDRequestState##bits)                   \
      COPY_REQUEST_STATE(flavor, stateCount, state)                      \
    }

#  define COPY_EXCEPTION_REQUEST_STATE_IDENTITY(bits)                      \
    static void CopyExceptionRequestStateIdentity##bits(                   \
        ExceptionRequest64& src, ExceptionRequestStateIdentity##bits& dst, \
        thread_state_flavor_t flavor, mach_msg_type_number_t stateCount,   \
        thread_state_t state) {                                            \
      COPY_REQUEST_COMMON(bits, sIDRequestStateIdentity##bits)             \
      COPY_REQUEST_IDENTITY                                                \
      COPY_REQUEST_STATE(flavor, stateCount, state)                        \
    }

COPY_EXCEPTION_REQUEST(32)
COPY_EXCEPTION_REQUEST_STATE(32)
COPY_EXCEPTION_REQUEST_STATE_IDENTITY(32)
COPY_EXCEPTION_REQUEST(64)
COPY_EXCEPTION_REQUEST_STATE(64)
COPY_EXCEPTION_REQUEST_STATE_IDENTITY(64)

#  undef COPY_EXCEPTION_REQUEST_STATE_IDENTITY
#  undef COPY_EXCEPTION_REQUEST_STATE
#  undef COPY_EXCEPTION_REQUEST
#  undef COPY_REQUEST_STATE
#  undef COPY_REQUEST_IDENTITY
#  undef COPY_REQUEST_COMMON

/* -------------------------------------------------------------------------- */
/*                 End Mach definitions and helper functions                  */
/* -------------------------------------------------------------------------- */

/* Every Mach exception handler is parameterized by these four properties. */
struct MachExceptionParameters {
  exception_mask_t mask;
  mach_port_t port;
  exception_behavior_t behavior;
  thread_state_flavor_t flavor;
};

struct ExceptionHandlerState {
  MachExceptionParameters current;
  MachExceptionParameters previous;

  /* Each Mach exception handler runs in its own thread. */
  Thread handlerThread;
};

/* This choice of ID is arbitrary, but must not match our exception ID. */
static const mach_msg_id_t sIDQuit = 42;

static ExceptionHandlerState* sMachExceptionState = nullptr;

/*
 * The meat of our exception handler. This thread waits for an exception
 * message, annotates the exception if needed, then forwards it to the
 * previously installed handler (which will likely terminate the process).
 */
static void MachExceptionHandler() {
  ThisThread::SetName("JS MachExceptionHandler");
  kern_return_t ret;
  MachExceptionParameters& current = sMachExceptionState->current;
  MachExceptionParameters& previous = sMachExceptionState->previous;

  // We use the simplest kind of 64-bit exception message here.
  ExceptionRequest64 request = {};
  request.header.msgh_local_port = current.port;
  request.header.msgh_size = static_cast<mach_msg_size_t>(sizeof(request));
  ret = mach_msg(&request.header, MACH_RCV_MSG, 0, request.header.msgh_size,
                 current.port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

  // Restore the previous handler. We're going to forward to it
  // anyway, and if we crash while doing so we don't want to hang.
  task_set_exception_ports(mach_task_self(), previous.mask, previous.port,
                           previous.behavior, previous.flavor);

  // If we failed even receiving the message, just give up.
  if (ret != MACH_MSG_SUCCESS) {
    MOZ_CRASH("MachExceptionHandler: mach_msg failed to receive a message!");
  }

  // Terminate the thread if we're shutting down.
  if (request.header.msgh_id == sIDQuit) {
    return;
  }

  // The only other valid message ID is the one associated with the
  // EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES behavior we chose.
  if (request.header.msgh_id != sIDRequest64) {
    MOZ_CRASH("MachExceptionHandler: Unexpected Message ID!");
  }

  // Make sure we can understand the exception we received.
  if (request.exception != EXC_BAD_ACCESS || request.code_count != 2) {
    MOZ_CRASH("MachExceptionHandler: Unexpected exception type!");
  }

  // Get the address that the offending code tried to access.
  uintptr_t address = uintptr_t(request.code[1]);

  // If the faulting address is inside one of our protected regions, we
  // want to annotate the crash to make it stand out from the crowd.
  if (sProtectedRegionsInit && sProtectedRegions.isProtected(address)) {
    ReportCrashIfDebug("Hit MOZ_CRASH(Tried to access a protected region!)\n");
    MOZ_CRASH_ANNOTATE("MOZ_CRASH(Tried to access a protected region!)");
  }

  // Forward to the previous handler which may be a debugger, the unix
  // signal handler, the crash reporter or something else entirely.
  if (previous.port != MACH_PORT_NULL) {
    mach_msg_type_number_t stateCount;
    thread_state_data_t state;
    if ((uint32_t(previous.behavior) & ~MACH_EXCEPTION_CODES) !=
        EXCEPTION_DEFAULT) {
      // If the previous handler requested thread state, get it here.
      stateCount = THREAD_STATE_MAX;
      ret = thread_get_state(request.thread.name, previous.flavor, state,
                             &stateCount);
      if (ret != KERN_SUCCESS) {
        MOZ_CRASH(
            "MachExceptionHandler: Could not get the thread state to forward!");
      }
    }

    // Depending on the behavior of the previous handler, the forwarded
    // exception message will have a different set of fields.
    // Of particular note is that exception handlers that lack
    // MACH_EXCEPTION_CODES will get 32-bit fields even on 64-bit
    // systems. It appears that OSX simply truncates these fields.
    ExceptionRequestUnion forward;
    switch (uint32_t(previous.behavior)) {
      case EXCEPTION_DEFAULT:
        CopyExceptionRequest32(request, forward.r32);
        break;
      case EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES:
        CopyExceptionRequest64(request, forward.r64);
        break;
      case EXCEPTION_STATE:
        CopyExceptionRequestState32(request, forward.rs32, previous.flavor,
                                    stateCount, state);
        break;
      case EXCEPTION_STATE | MACH_EXCEPTION_CODES:
        CopyExceptionRequestState64(request, forward.rs64, previous.flavor,
                                    stateCount, state);
        break;
      case EXCEPTION_STATE_IDENTITY:
        CopyExceptionRequestStateIdentity32(request, forward.rsi32,
                                            previous.flavor, stateCount, state);
        break;
      case EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES:
        CopyExceptionRequestStateIdentity64(request, forward.rsi64,
                                            previous.flavor, stateCount, state);
        break;
      default:
        MOZ_CRASH("MachExceptionHandler: Unknown previous handler behavior!");
    }

    // Forward the generated message to the old port. The local and remote
    // port fields *and their rights* are swapped on arrival, so we need to
    // swap them back first.
    forward.header.msgh_bits =
        (request.header.msgh_bits & ~MACH_MSGH_BITS_PORTS_MASK) |
        MACH_MSGH_BITS(MACH_MSGH_BITS_LOCAL(request.header.msgh_bits),
                       MACH_MSGH_BITS_REMOTE(request.header.msgh_bits));
    forward.header.msgh_local_port = forward.header.msgh_remote_port;
    forward.header.msgh_remote_port = previous.port;
    ret = mach_msg(&forward.header, MACH_SEND_MSG, forward.header.msgh_size, 0,
                   MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (ret != MACH_MSG_SUCCESS) {
      MOZ_CRASH(
          "MachExceptionHandler: Failed to forward to the previous handler!");
    }
  } else {
    // There was no previous task-level exception handler, so defer to the
    // host level one instead. We set the return code to KERN_FAILURE to
    // indicate that we did not handle the exception.
    // The reply message ID is always the request ID + 100.
    ExceptionReply reply = {};
    reply.header.msgh_bits =
        MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.header.msgh_bits), 0);
    reply.header.msgh_size = static_cast<mach_msg_size_t>(sizeof(reply));
    reply.header.msgh_remote_port = request.header.msgh_remote_port;
    reply.header.msgh_local_port = MACH_PORT_NULL;
    reply.header.msgh_id = request.header.msgh_id + 100;
    reply.NDR = request.NDR;
    reply.RetCode = KERN_FAILURE;
    ret = mach_msg(&reply.header, MACH_SEND_MSG, reply.header.msgh_size, 0,
                   MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (ret != MACH_MSG_SUCCESS) {
      MOZ_CRASH("MachExceptionHandler: Failed to forward to the host level!");
    }
  }
}

static void TerminateMachExceptionHandlerThread() {
  // Send a simple quit message to the exception handler thread.
  mach_msg_header_t msg;
  msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  msg.msgh_size = static_cast<mach_msg_size_t>(sizeof(msg));
  msg.msgh_remote_port = sMachExceptionState->current.port;
  msg.msgh_local_port = MACH_PORT_NULL;
  msg.msgh_reserved = 0;
  msg.msgh_id = sIDQuit;
  kern_return_t ret =
      mach_msg(&msg, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL,
               MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

  if (ret == MACH_MSG_SUCCESS) {
    sMachExceptionState->handlerThread.join();
  } else {
    MOZ_CRASH("MachExceptionHandler: Handler thread failed to terminate!");
  }
}

bool MemoryProtectionExceptionHandler::install() {
  MOZ_ASSERT(!sExceptionHandlerInstalled);
  MOZ_ASSERT(!sMachExceptionState);

  // If the exception handler is disabled, report success anyway.
  if (MemoryProtectionExceptionHandler::isDisabled()) {
    return true;
  }

  sMachExceptionState = js_new<ExceptionHandlerState>();
  if (!sMachExceptionState) {
    return false;
  }

  kern_return_t ret;
  mach_port_t task = mach_task_self();

  // Allocate a new exception port with receive rights.
  sMachExceptionState->current = {};
  MachExceptionParameters& current = sMachExceptionState->current;
  ret = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &current.port);
  if (ret != KERN_SUCCESS) {
    return false;
  }

  // Give the new port send rights as well.
  ret = mach_port_insert_right(task, current.port, current.port,
                               MACH_MSG_TYPE_MAKE_SEND);
  if (ret != KERN_SUCCESS) {
    mach_port_deallocate(task, current.port);
    current = {};
    return false;
  }

  // Start the thread that will receive the messages from our exception port.
  if (!sMachExceptionState->handlerThread.init(MachExceptionHandler)) {
    mach_port_deallocate(task, current.port);
    current = {};
    return false;
  }

  // Set the other properties of our new exception handler.
  current.mask = EXC_MASK_BAD_ACCESS;
  current.behavior =
      exception_behavior_t(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES);
  current.flavor = THREAD_STATE_NONE;

  // Tell the task to use our exception handler, and save the previous one.
  sMachExceptionState->previous = {};
  MachExceptionParameters& previous = sMachExceptionState->previous;
  mach_msg_type_number_t previousCount = 1;
  ret = task_swap_exception_ports(
      task, current.mask, current.port, current.behavior, current.flavor,
      &previous.mask, &previousCount, &previous.port, &previous.behavior,
      &previous.flavor);
  if (ret != KERN_SUCCESS) {
    TerminateMachExceptionHandlerThread();
    mach_port_deallocate(task, current.port);
    previous = {};
    current = {};
    return false;
  }

  // We should have info on the previous exception handler, even if it's null.
  MOZ_ASSERT(previousCount == 1);

  sExceptionHandlerInstalled = true;
  return sExceptionHandlerInstalled;
}

void MemoryProtectionExceptionHandler::uninstall() {
  if (sExceptionHandlerInstalled) {
    MOZ_ASSERT(sMachExceptionState);

    mach_port_t task = mach_task_self();

    // Restore the previous exception handler.
    MachExceptionParameters& previous = sMachExceptionState->previous;
    task_set_exception_ports(task, previous.mask, previous.port,
                             previous.behavior, previous.flavor);

    TerminateMachExceptionHandlerThread();

    // Release the Mach IPC port we used.
    mach_port_deallocate(task, sMachExceptionState->current.port);

    sMachExceptionState->current = {};
    sMachExceptionState->previous = {};

    js_delete(sMachExceptionState);
    sMachExceptionState = nullptr;

    sExceptionHandlerInstalled = false;
  }
}

#else

#  error "This platform is not supported!"

#endif

} /* namespace js */
