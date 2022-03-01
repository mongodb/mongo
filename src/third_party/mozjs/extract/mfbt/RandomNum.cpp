/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "mozilla/RandomNum.h"

#include <fcntl.h>
#ifdef XP_UNIX
#  include <unistd.h>
#endif

#if defined(XP_WIN)

// Microsoft doesn't "officially" support using RtlGenRandom() directly
// anymore, and the Windows headers assume that __stdcall is
// the default calling convention (which is true when Microsoft uses this
// function to build their own CRT libraries).

// We will explicitly declare it with the proper calling convention.

#  include "minwindef.h"
#  define RtlGenRandom SystemFunction036
extern "C" BOOLEAN NTAPI RtlGenRandom(PVOID RandomBuffer,
                                      ULONG RandomBufferLength);

#endif

#if defined(ANDROID) || defined(XP_DARWIN) || defined(__DragonFly__) || \
    defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  include <stdlib.h>
#  define USE_ARC4RANDOM
#endif

#if defined(__linux__)
#  include <linux/random.h>  // For GRND_NONBLOCK.
#  include <sys/syscall.h>   // For SYS_getrandom.

// Older glibc versions don't define SYS_getrandom, so we define it here if
// it's not available. See bug 995069.
#  if defined(__x86_64__)
#    define GETRANDOM_NR 318
#  elif defined(__i386__)
#    define GETRANDOM_NR 355
#  elif defined(__aarch64__)
#    define GETRANDOM_NR 278
#  elif defined(__arm__)
#    define GETRANDOM_NR 384
#  elif defined(__powerpc__)
#    define GETRANDOM_NR 359
#  elif defined(__s390__)
#    define GETRANDOM_NR 349
#  elif defined(__mips__)
#    include <sgidefs.h>
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define GETRANDOM_NR 4353
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define GETRANDOM_NR 5313
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define GETRANDOM_NR 6317
#    endif
#  endif

#  if defined(SYS_getrandom)
// We have SYS_getrandom. Use it to check GETRANDOM_NR. Only do this if we set
// GETRANDOM_NR so tier 3 platforms with recent glibc are not forced to define
// it for no good reason.
#    if defined(GETRANDOM_NR)
static_assert(GETRANDOM_NR == SYS_getrandom,
              "GETRANDOM_NR should match the actual SYS_getrandom value");
#    endif
#  else
#    define SYS_getrandom GETRANDOM_NR
#  endif

#  if defined(GRND_NONBLOCK)
static_assert(GRND_NONBLOCK == 1,
              "If GRND_NONBLOCK is not 1 the #define below is wrong");
#  else
#    define GRND_NONBLOCK 1
#  endif

#endif  // defined(__linux__)

namespace mozilla {

/*
 * Note - Bug 1500115 has been opened to discuss simplifying or improving
 * this function in the future; however, the function is secure as-is right
 * now. Further improvements may be made to reduce complexity, improve
 * robustness, or take advantage of OS-specific API improvements as they
 * become available.
 *
 */

MFBT_API Maybe<uint64_t> RandomUint64() {
#if defined(XP_WIN)

  uint64_t result = 0;
  if (!RtlGenRandom(&result, sizeof(result))) {
    return Nothing();
  }

  return Some(result);

#elif defined(USE_ARC4RANDOM)  // defined(XP_WIN)

  return Some((static_cast<uint64_t>(arc4random()) << 32) | arc4random());

#elif defined(XP_UNIX)  // defined(USE_ARC4RANDOM)

  uint64_t result = 0;

#  if defined(__linux__)

  long bytesGenerated =
      syscall(SYS_getrandom, &result, sizeof(result), GRND_NONBLOCK);

  if ((bytesGenerated > 0) &&
      (static_cast<unsigned long>(bytesGenerated) == sizeof(result))) {
    return Some(result);
  }

  // Fall-through to UNIX behavior if failed

#  endif  // defined(__linux__)

  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return Nothing();
  }

  ssize_t bytesRead = read(fd, &result, sizeof(result));

  close(fd);

  if (bytesRead < 0) {
    return Nothing();
  }

  if (static_cast<size_t>(bytesRead) != sizeof(result)) {
    return Nothing();
  }

  return Some(result);

#else  // defined(XP_UNIX)
#  error "Platform needs to implement RandomUint64()"
#endif
}

MFBT_API uint64_t RandomUint64OrDie() {
  Maybe<uint64_t> maybeRandomNum = RandomUint64();

  MOZ_RELEASE_ASSERT(maybeRandomNum.isSome());

  return maybeRandomNum.value();
}

}  // namespace mozilla
