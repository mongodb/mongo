/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Additional definitions and implementation for fuzzing code */

#ifndef mozilla_Fuzzing_h
#define mozilla_Fuzzing_h

#ifdef FUZZING_SNAPSHOT
#  include "mozilla/fuzzing/NyxWrapper.h"

#  ifdef __cplusplus
#    include "mozilla/fuzzing/Nyx.h"
#    include "mozilla/ScopeExit.h"

#    define MOZ_FUZZING_NYX_RELEASE(id)                       \
      if (mozilla::fuzzing::Nyx::instance().is_enabled(id)) { \
        mozilla::fuzzing::Nyx::instance().release();          \
      }

#    define MOZ_FUZZING_NYX_GUARD(id)                           \
      auto nyxGuard = mozilla::MakeScopeExit([&] {              \
        if (mozilla::fuzzing::Nyx::instance().is_enabled(id)) { \
          mozilla::fuzzing::Nyx::instance().release();          \
        }                                                       \
      });
#  endif

#  define MOZ_FUZZING_HANDLE_CRASH_EVENT2(aType, aReason)     \
    do {                                                      \
      if (nyx_handle_event) {                                 \
        nyx_handle_event(aType, __FILE__, __LINE__, aReason); \
      }                                                       \
    } while (false)

#  define MOZ_FUZZING_HANDLE_CRASH_EVENT4(aType, aFilename, aLine, aReason) \
    do {                                                                    \
      if (nyx_handle_event) {                                               \
        nyx_handle_event(aType, aFilename, aLine, aReason);                 \
      }                                                                     \
    } while (false)

#  define MOZ_FUZZING_NYX_PRINT(aMsg) \
    do {                              \
      if (nyx_puts) {                 \
        nyx_puts(aMsg);               \
      } else {                        \
        fprintf(stderr, aMsg);        \
      }                               \
    } while (false)

#  define MOZ_FUZZING_NYX_PRINTF(aFormat, ...)                         \
    do {                                                               \
      if (nyx_puts) {                                                  \
        char msgbuf[2048];                                             \
        snprintf(msgbuf, sizeof(msgbuf) - 1, "" aFormat, __VA_ARGS__); \
        nyx_puts(msgbuf);                                              \
      } else {                                                         \
        fprintf(stderr, aFormat, __VA_ARGS__);                         \
      }                                                                \
    } while (false)

#  ifdef FUZZ_DEBUG
#    define MOZ_FUZZING_NYX_DEBUG(x) MOZ_FUZZING_NYX_PRINT(x)
#  else
#    define MOZ_FUZZING_NYX_DEBUG(x)
#  endif
#  define MOZ_FUZZING_NYX_ABORT(aMsg) \
    do {                              \
      MOZ_FUZZING_NYX_PRINT(aMsg);    \
      MOZ_REALLY_CRASH(__LINE__);     \
    } while (false);
#else
#  define MOZ_FUZZING_NYX_RELEASE(id)
#  define MOZ_FUZZING_NYX_GUARD(id)
#  define MOZ_FUZZING_NYX_PRINT(aMsg)
#  define MOZ_FUZZING_NYX_PRINTF(aFormat, ...)
#  define MOZ_FUZZING_NYX_DEBUG(aMsg)
#  define MOZ_FUZZING_NYX_ABORT(aMsg)
#  define MOZ_FUZZING_HANDLE_CRASH_EVENT2(aType, aReason) \
    do {                                                  \
    } while (false)
#  define MOZ_FUZZING_HANDLE_CRASH_EVENT4(aType, aFilename, aLine, aReason) \
    do {                                                                    \
    } while (false)
#endif

#endif /* mozilla_Fuzzing_h */
