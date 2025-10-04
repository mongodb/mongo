// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/segv_handler.h"

#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/debugging/stacktrace.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/guarded_allocations.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

constexpr const char* WriteFlagToString(WriteFlag write_flag) {
  switch (write_flag) {
    case WriteFlag::Unknown:
      return "(read or write: indeterminate)";
    case WriteFlag::Read:
      return "(read)";
    case WriteFlag::Write:
      return "(write)";
  }
  ASSUME(false);
}

#if defined(__aarch64__)
struct __esr_context {
  struct _aarch64_ctx head;
  uint64_t esr;
};

static bool Aarch64GetESR(const ucontext_t* ucontext, uint64_t* esr) {
  static const uint32_t kEsrMagic = 0x45535201;
  const uint8_t* aux =
      reinterpret_cast<const uint8_t*>(ucontext->uc_mcontext.__reserved);
  while (true) {
    const _aarch64_ctx* ctx = (const _aarch64_ctx*)aux;
    if (ctx->size == 0) break;
    if (ctx->magic == kEsrMagic) {
      *esr = ((const __esr_context*)ctx)->esr;
      return true;
    }
    aux += ctx->size;
  }
  return false;
}
#endif

static WriteFlag ExtractWriteFlagFromContext(const void* context) {
  if (context == nullptr) {
    return WriteFlag::Unknown;
  }
#if defined(__x86_64__)
  const ucontext_t* uc = reinterpret_cast<const ucontext_t*>(context);
  uintptr_t value = uc->uc_mcontext.gregs[REG_ERR];
  static const uint64_t PF_WRITE = 1U << 1;
  return value & PF_WRITE ? WriteFlag::Write : WriteFlag::Read;
#elif defined(__aarch64__)
  const ucontext_t* uc = reinterpret_cast<const ucontext_t*>(context);
  uint64_t esr;
  if (!Aarch64GetESR(uc, &esr)) return WriteFlag::Unknown;
  static const uint64_t ESR_ELx_WNR = 1U << 6;
  return esr & ESR_ELx_WNR ? WriteFlag::Write : WriteFlag::Read;
#else
  // __riscv is NOT (yet) supported
  return WriteFlag::Unknown;
#endif
}

static GuardedAllocationsErrorType RefineErrorTypeBasedOnWriteFlag(
    GuardedAllocationsErrorType error, WriteFlag write_flag) {
  switch (error) {
    case GuardedAllocationsErrorType::kUseAfterFree:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kUseAfterFreeWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kUseAfterFreeRead;
        default:
          break;
      }
      break;
    case GuardedAllocationsErrorType::kBufferUnderflow:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kBufferUnderflowWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kBufferUnderflowRead;
        default:
          break;
      }
      break;
    case GuardedAllocationsErrorType::kBufferOverflow:
      switch (write_flag) {
        case WriteFlag::Write:
          return GuardedAllocationsErrorType::kBufferOverflowWrite;
        case WriteFlag::Read:
          return GuardedAllocationsErrorType::kBufferOverflowRead;
        default:
          break;
      }
      break;
    default:
      break;
  }
  return error;
}

GuardedAllocationsErrorType RefineErrorTypeBasedOnContext(
    const void* context, GuardedAllocationsErrorType error) {
  WriteFlag write_flag = ExtractWriteFlagFromContext(context);
  return RefineErrorTypeBasedOnWriteFlag(error, write_flag);
}

// This is overridden by selsan, if it's linked in.
ABSL_ATTRIBUTE_WEAK void SelsanTrapHandler(void* info, void* ctx) {}

// A SEGV handler that prints stack traces for the allocation and deallocation
// of relevant memory as well as the location of the memory error.
void SegvHandler(int signo, siginfo_t* info, void* context) {
  if (signo == SIGTRAP) {
    SelsanTrapHandler(info, context);
  }
  if (signo != SIGSEGV) return;
  void* fault = info->si_addr;
  if (!tc_globals.guardedpage_allocator().PointerIsMine(fault)) return;

  GuardedAllocationsStackTrace *alloc_trace, *dealloc_trace;
  GuardedAllocationsErrorType error =
      tc_globals.guardedpage_allocator().GetStackTraces(fault, &alloc_trace,
                                                        &dealloc_trace);
  if (error == GuardedAllocationsErrorType::kUnknown) return;
  WriteFlag write_flag = ExtractWriteFlagFromContext(context);
  error = RefineErrorTypeBasedOnWriteFlag(error, write_flag);
  pid_t current_thread = absl::base_internal::GetTID();
  off_t offset;
  size_t size;
  std::tie(offset, size) =
      tc_globals.guardedpage_allocator().GetAllocationOffsetAndSize(fault);

  TC_LOG("*** GWP-ASan (https://google.github.io/tcmalloc/gwp-asan.html) has detected a memory error ***");
  TC_LOG(">>> Access at offset %v into buffer of length %v", offset, size);
  TC_LOG("Error originates from memory allocated in thread %v at:",
         alloc_trace->thread_id);
  PrintStackTrace(alloc_trace->stack, alloc_trace->depth);

  switch (error) {
    case GuardedAllocationsErrorType::kUseAfterFree:
    case GuardedAllocationsErrorType::kUseAfterFreeRead:
    case GuardedAllocationsErrorType::kUseAfterFreeWrite:
      TC_LOG("The memory was freed in thread %v at:", dealloc_trace->thread_id);
      PrintStackTrace(dealloc_trace->stack, dealloc_trace->depth);
      TC_LOG("Use-after-free %s occurs in thread %v at:",
             WriteFlagToString(write_flag), current_thread);
      RecordCrash("GWP-ASan", "use-after-free");
      break;
    case GuardedAllocationsErrorType::kBufferUnderflow:
    case GuardedAllocationsErrorType::kBufferUnderflowRead:
    case GuardedAllocationsErrorType::kBufferUnderflowWrite:
      TC_LOG("Buffer underflow %s occurs in thread %v at:",
             WriteFlagToString(write_flag), current_thread);
      RecordCrash("GWP-ASan", "buffer-underflow");
      break;
    case GuardedAllocationsErrorType::kBufferOverflow:
    case GuardedAllocationsErrorType::kBufferOverflowRead:
    case GuardedAllocationsErrorType::kBufferOverflowWrite:
      TC_LOG("Buffer overflow %s occurs in thread %v at:",
             WriteFlagToString(write_flag), current_thread);
      RecordCrash("GWP-ASan", "buffer-overflow");
      break;
    case GuardedAllocationsErrorType::kDoubleFree:
      TC_LOG("The memory was freed in thread %v at:", dealloc_trace->thread_id);
      PrintStackTrace(dealloc_trace->stack, dealloc_trace->depth);
      TC_LOG("Double free occurs in thread %v at:", current_thread);
      RecordCrash("GWP-ASan", "double-free");
      break;
    case GuardedAllocationsErrorType::kBufferOverflowOnDealloc:
      TC_LOG("Buffer overflow (write) detected in thread %v at free:",
             current_thread);
      RecordCrash("GWP-ASan", "buffer-overflow-detected-at-free");
      break;
    case GuardedAllocationsErrorType::kUnknown:
      TC_BUG("Unexpected GuardedAllocationsErrorType::kUnknown");
  }
  PrintStackTraceFromSignalHandler(context);
  if (error == GuardedAllocationsErrorType::kBufferOverflowOnDealloc) {
    TC_LOG(
        "*** Try rerunning with --config=asan to get stack trace of overflow "
        "***");
  }
}

static struct sigaction old_segv_sa;
static struct sigaction old_trap_sa;

static void ForwardSignal(int signo, siginfo_t* info, void* context) {
  auto& old_sa = signo == SIGSEGV ? old_segv_sa : old_trap_sa;
  if (old_sa.sa_flags & SA_SIGINFO) {
    old_sa.sa_sigaction(signo, info, context);
  } else if (old_sa.sa_handler == SIG_DFL) {
    // No previous handler registered.  Re-raise signal for core dump.
    int err = sigaction(signo, &old_sa, nullptr);
    if (err == -1) {
      TC_LOG("Couldn't restore previous sigaction!");
    }
    raise(signo);
  } else if (old_sa.sa_handler == SIG_IGN) {
    return;  // Previous sigaction ignored signal, so do the same.
  } else {
    old_sa.sa_handler(signo);
  }
}

static void HandleSegvAndForward(int signo, siginfo_t* info, void* context) {
  SegvHandler(signo, info, context);
  ForwardSignal(signo, info, context);
}

extern "C" void MallocExtension_Internal_ActivateGuardedSampling() {
  static absl::once_flag flag;
  absl::base_internal::LowLevelCallOnce(&flag, []() {
    struct sigaction action = {};
    action.sa_sigaction = HandleSegvAndForward;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &old_segv_sa);
    sigaction(SIGTRAP, &action, &old_trap_sa);
    tc_globals.guardedpage_allocator().AllowAllocations();
  });
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
