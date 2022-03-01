/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MmapFaultHandler.h"

#if defined(XP_UNIX) && !defined(XP_DARWIN) && !defined(__wasi__)

#  include "PlatformMutex.h"
#  include "mozilla/Atomics.h"
#  include "mozilla/MemoryChecking.h"
#  include "mozilla/ThreadLocal.h"
#  include <signal.h>
#  include <cstring>

static MOZ_THREAD_LOCAL(MmapAccessScope*) sMmapAccessScope;

static struct sigaction sPrevSIGBUSHandler;

static void MmapSIGBUSHandler(int signum, siginfo_t* info, void* context) {
  MOZ_RELEASE_ASSERT(signum == SIGBUS);

  MmapAccessScope* mas = sMmapAccessScope.get();

  if (mas && mas->IsInsideBuffer(info->si_addr)) {
    // Temporarily instead of handling the signal, we crash intentionally and
    // send some diagnostic information to find out why the signal is received.
    mas->CrashWithInfo(info->si_addr);

    // The address is inside the buffer, handle the failure.
    siglongjmp(mas->mJmpBuf, signum);
  }

  // This signal is not caused by accessing region protected by MmapAccessScope.
  // Forward the signal to the next handler.
  if (sPrevSIGBUSHandler.sa_flags & SA_SIGINFO) {
    sPrevSIGBUSHandler.sa_sigaction(signum, info, context);
  } else if (sPrevSIGBUSHandler.sa_handler == SIG_DFL ||
             sPrevSIGBUSHandler.sa_handler == SIG_IGN) {
    // There is no next handler. Uninstalling our handler and returning will
    // cause a crash.
    sigaction(signum, &sPrevSIGBUSHandler, nullptr);
  } else {
    sPrevSIGBUSHandler.sa_handler(signum);
  }
}

mozilla::Atomic<bool> gSIGBUSHandlerInstalled(false);
mozilla::Atomic<bool> gSIGBUSHandlerInstalling(false);

void InstallMmapFaultHandler() {
  // This function is called from MmapAccessScope's constructor because there is
  // no single point where we could install the handler during startup. This
  // means that it's called quite often, so to minimize using of the mutex we
  // first check the atomic variable outside the lock.
  if (gSIGBUSHandlerInstalled) {
    return;
  }

  if (gSIGBUSHandlerInstalling.compareExchange(false, true)) {
    sMmapAccessScope.infallibleInit();

    struct sigaction busHandler;
    busHandler.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    busHandler.sa_sigaction = MmapSIGBUSHandler;
    sigemptyset(&busHandler.sa_mask);
    if (sigaction(SIGBUS, &busHandler, &sPrevSIGBUSHandler)) {
      MOZ_CRASH("Unable to install SIGBUS handler");
    }

    MOZ_ASSERT(!gSIGBUSHandlerInstalled);
    gSIGBUSHandlerInstalled = true;
  } else {
    // Just spin lock here. It should not take a substantial amount
    // of time, so a mutex would likely be a spin lock anyway, and
    // this avoids the need to new up a static mutex from within
    // mozglue/misc, which complicates things with
    // check_vanilla_allocations.py
    while (!gSIGBUSHandlerInstalled) {
    }
  }
}

MmapAccessScope::MmapAccessScope(void* aBuf, uint32_t aBufLen,
                                 const char* aFilename) {
  // Install signal handler if it wasn't installed yet.
  InstallMmapFaultHandler();

  // We'll handle the signal only if the crashing address is inside this buffer.
  mBuf = aBuf;
  mBufLen = aBufLen;
  mFilename = aFilename;

  SetThreadLocalScope();
}

MmapAccessScope::~MmapAccessScope() {
  MOZ_RELEASE_ASSERT(sMmapAccessScope.get() == this);
  sMmapAccessScope.set(mPreviousScope);
}

void MmapAccessScope::SetThreadLocalScope() {
  // mJmpBuf is set outside of this classs for reasons mentioned in the header
  // file, but we need to initialize the member here too to make Coverity happy.
  memset(mJmpBuf, 0, sizeof(sigjmp_buf));

  // If MmapAccessScopes are nested, save the previous one and restore it in
  // the destructor.
  mPreviousScope = sMmapAccessScope.get();

  // MmapAccessScope is now set up (except mJmpBuf for reasons mentioned in the
  // header file). Store the pointer in a thread-local variable sMmapAccessScope
  // so we can use it in the handler if the signal is triggered.
  sMmapAccessScope.set(this);
}

bool MmapAccessScope::IsInsideBuffer(void* aPtr) {
  return aPtr >= mBuf && aPtr < (void*)((char*)mBuf + mBufLen);
}

void MmapAccessScope::CrashWithInfo(void* aPtr) {
  // All we have is the buffer and the crashing address.
  MOZ_CRASH_UNSAFE_PRINTF(
      "SIGBUS received when accessing mmaped file [buffer=%p, "
      "buflen=%u, address=%p, filename=%s]",
      mBuf, mBufLen, aPtr, mFilename);
}

#endif
