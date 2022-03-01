/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/NativeStack.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_RELEASE_ASSERT, MOZ_CRASH

#ifdef XP_WIN
#  include "util/Windows.h"
#elif defined(__wasi__)
// Nothing
#elif defined(XP_DARWIN) || defined(DARWIN) || defined(XP_UNIX)
#  include <pthread.h>
#  if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#    include <pthread_np.h>
#  endif
#  if defined(SOLARIS) || defined(AIX)
#    include <ucontext.h>
#  endif
#  if defined(ANDROID) && !defined(__aarch64__)
#    include <sys/types.h>
#    include <unistd.h>
#  endif
#  if defined(XP_LINUX) && !defined(ANDROID) && defined(__GLIBC__)
#    include <dlfcn.h>
#    include <sys/syscall.h>
#    include <sys/types.h>
#    include <unistd.h>
#    define gettid() static_cast<pid_t>(syscall(__NR_gettid))
#  endif
#else
#  error "Unsupported platform"
#endif

#include "js/friend/StackLimits.h"  // JS_STACK_GROWTH_DIRECTION

#if defined(XP_WIN)

void* js::GetNativeStackBaseImpl() {
  PNT_TIB pTib = reinterpret_cast<PNT_TIB>(NtCurrentTeb());
  return static_cast<void*>(pTib->StackBase);
}

#elif defined(SOLARIS)

static_assert(JS_STACK_GROWTH_DIRECTION < 0);

void* js::GetNativeStackBaseImpl() {
  stack_t st;
  stack_getbounds(&st);
  return static_cast<char*>(st.ss_sp) + st.ss_size;
}

#elif defined(AIX)

static_assert(JS_STACK_GROWTH_DIRECTION < 0);

void* js::GetNativeStackBaseImpl() {
  ucontext_t context;
  getcontext(&context);
  return static_cast<char*>(context.uc_stack.ss_sp) + context.uc_stack.ss_size;
}

#elif defined(XP_LINUX) && !defined(ANDROID) && defined(__GLIBC__)
void* js::GetNativeStackBaseImpl() {
  // On the main thread, get stack base from glibc's __libc_stack_end rather
  // than pthread APIs to avoid filesystem calls /proc/self/maps.  Non-main
  // threads spawned with pthreads can read this information directly from their
  // pthread struct, but the main thread must go parse /proc/self/maps to figure
  // the mapped stack address space ranges.  We want to avoid reading from
  // /proc/ so that firefox can run in sandboxed environments where /proc may
  // not be mounted.
  if (gettid() == getpid()) {
    void** pLibcStackEnd = (void**)dlsym(RTLD_DEFAULT, "__libc_stack_end");

    // If __libc_stack_end is not found, architecture specific frame pointer
    // hopping will need to be implemented.
    MOZ_RELEASE_ASSERT(
        pLibcStackEnd,
        "__libc_stack_end unavailable, unable to setup stack range for JS");
    void* stackBase = *pLibcStackEnd;
    MOZ_RELEASE_ASSERT(
        stackBase, "invalid stack base, unable to setup stack range for JS");

    // We don't need to fix stackBase, as it already roughly points to beginning
    // of the stack
    return stackBase;
  }

  // Non-main threads have the required info stored in memory, so no filesystem
  // calls are made.
  pthread_t thread = pthread_self();
  pthread_attr_t sattr;
  pthread_attr_init(&sattr);
  pthread_getattr_np(thread, &sattr);

  // stackBase will be the *lowest* address on all architectures.
  void* stackBase = nullptr;
  size_t stackSize = 0;
  int rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
  if (rc) {
    MOZ_CRASH(
        "call to pthread_attr_getstack failed, unable to setup stack range for "
        "JS");
  }
  MOZ_RELEASE_ASSERT(stackBase,
                     "invalid stack base, unable to setup stack range for JS");
  pthread_attr_destroy(&sattr);

#  if JS_STACK_GROWTH_DIRECTION > 0
  return stackBase;
#  else
  return static_cast<char*>(stackBase) + stackSize;
#  endif
}

#elif defined(__wasi__)

// Since we rearrange the layout for wasi via --stack-first flag for the linker
// the final layout is: 0x00 | <- stack | data | heap -> |.
static void* const NativeStackBase = __builtin_frame_address(0);

void* js::GetNativeStackBaseImpl() {
  MOZ_ASSERT(JS_STACK_GROWTH_DIRECTION < 0);
  return NativeStackBase;
}

#else  // __wasi__

void* js::GetNativeStackBaseImpl() {
  pthread_t thread = pthread_self();
#  if defined(XP_DARWIN) || defined(DARWIN)
  return pthread_get_stackaddr_np(thread);

#  else
  pthread_attr_t sattr;
  pthread_attr_init(&sattr);
#    if defined(__OpenBSD__)
  stack_t ss;
#    elif defined(PTHREAD_NP_H) || defined(_PTHREAD_NP_H_) || defined(NETBSD)
  /* e.g. on FreeBSD 4.8 or newer, neundorf@kde.org */
  pthread_attr_get_np(thread, &sattr);
#    else
  /*
   * FIXME: this function is non-portable;
   * other POSIX systems may have different np alternatives
   */
  pthread_getattr_np(thread, &sattr);
#    endif

  void* stackBase = 0;
  size_t stackSize = 0;
  int rc;
#    if defined(__OpenBSD__)
  rc = pthread_stackseg_np(pthread_self(), &ss);
  stackBase = (void*)((size_t)ss.ss_sp - ss.ss_size);
  stackSize = ss.ss_size;
#    elif defined(ANDROID) && !defined(__aarch64__)
  if (gettid() == getpid()) {
    // bionic's pthread_attr_getstack prior to API 21 doesn't tell the truth
    // for the main thread (see bug 846670). So we scan /proc/self/maps to
    // find the segment which contains the stack.
    rc = -1;

    // Put the string on the stack, otherwise there is the danger that it
    // has not been decompressed by the the on-demand linker. Bug 1165460.
    //
    // The volatile keyword should stop the compiler from trying to omit
    // the stack copy in the future (hopefully).
    volatile char path[] = "/proc/self/maps";
    FILE* fs = fopen((const char*)path, "r");

    if (fs) {
      char line[100];
      unsigned long stackAddr = (unsigned long)&sattr;
      while (fgets(line, sizeof(line), fs) != nullptr) {
        unsigned long stackStart;
        unsigned long stackEnd;
        if (sscanf(line, "%lx-%lx ", &stackStart, &stackEnd) == 2 &&
            stackAddr >= stackStart && stackAddr < stackEnd) {
          stackBase = (void*)stackStart;
          stackSize = stackEnd - stackStart;
          rc = 0;
          break;
        }
      }
      fclose(fs);
    }
  } else {
    // For non main-threads pthread allocates the stack itself so it tells
    // the truth.
    rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
  }
#    else
  rc = pthread_attr_getstack(&sattr, &stackBase, &stackSize);
#    endif
  if (rc) {
    MOZ_CRASH();
  }
  MOZ_ASSERT(stackBase);
  pthread_attr_destroy(&sattr);

#    if JS_STACK_GROWTH_DIRECTION > 0
  return stackBase;
#    else
  return static_cast<char*>(stackBase) + stackSize;
#    endif
#  endif
}

#endif /* !XP_WIN */
