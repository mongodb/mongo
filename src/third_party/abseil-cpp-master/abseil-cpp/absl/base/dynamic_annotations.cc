// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <string.h>

#include "absl/base/dynamic_annotations.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* Compiler-based ThreadSanitizer defines
   DYNAMIC_ANNOTATIONS_EXTERNAL_IMPL = 1
   and provides its own definitions of the functions. */

#ifndef DYNAMIC_ANNOTATIONS_EXTERNAL_IMPL
# define DYNAMIC_ANNOTATIONS_EXTERNAL_IMPL 0
#endif

/* Each function is empty and called (via a macro) only in debug mode.
   The arguments are captured by dynamic tools at runtime. */

#if DYNAMIC_ANNOTATIONS_EXTERNAL_IMPL == 0 && !defined(__native_client__)

#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void AnnotateRWLockCreate(const char *, int,
                          const volatile void *){}
void AnnotateRWLockDestroy(const char *, int,
                           const volatile void *){}
void AnnotateRWLockAcquired(const char *, int,
                            const volatile void *, long){}
void AnnotateRWLockReleased(const char *, int,
                            const volatile void *, long){}
void AnnotateBenignRace(const char *, int,
                        const volatile void *,
                        const char *){}
void AnnotateBenignRaceSized(const char *, int,
                             const volatile void *,
                             size_t,
                             const char *) {}
void AnnotateThreadName(const char *, int,
                        const char *){}
void AnnotateIgnoreReadsBegin(const char *, int){}
void AnnotateIgnoreReadsEnd(const char *, int){}
void AnnotateIgnoreWritesBegin(const char *, int){}
void AnnotateIgnoreWritesEnd(const char *, int){}
void AnnotateEnableRaceDetection(const char *, int, int){}
void AnnotateMemoryIsInitialized(const char *, int,
                                 const volatile void *mem, size_t size) {
#if __has_feature(memory_sanitizer)
  __msan_unpoison(mem, size);
#else
  (void)mem;
  (void)size;
#endif
}

void AnnotateMemoryIsUninitialized(const char *, int,
                                   const volatile void *mem, size_t size) {
#if __has_feature(memory_sanitizer)
  __msan_allocated_memory(mem, size);
#else
  (void)mem;
  (void)size;
#endif
}

static int GetRunningOnValgrind(void) {
#ifdef RUNNING_ON_VALGRIND
  if (RUNNING_ON_VALGRIND) return 1;
#endif
  char *running_on_valgrind_str = getenv("RUNNING_ON_VALGRIND");
  if (running_on_valgrind_str) {
    return strcmp(running_on_valgrind_str, "0") != 0;
  }
  return 0;
}

/* See the comments in dynamic_annotations.h */
int RunningOnValgrind(void) {
  static volatile int running_on_valgrind = -1;
  int local_running_on_valgrind = running_on_valgrind;
  /* C doesn't have thread-safe initialization of statics, and we
     don't want to depend on pthread_once here, so hack it. */
  ANNOTATE_BENIGN_RACE(&running_on_valgrind, "safe hack");
  if (local_running_on_valgrind == -1)
    running_on_valgrind = local_running_on_valgrind = GetRunningOnValgrind();
  return local_running_on_valgrind;
}

/* See the comments in dynamic_annotations.h */
double ValgrindSlowdown(void) {
  /* Same initialization hack as in RunningOnValgrind(). */
  static volatile double slowdown = 0.0;
  double local_slowdown = slowdown;
  ANNOTATE_BENIGN_RACE(&slowdown, "safe hack");
  if (RunningOnValgrind() == 0) {
    return 1.0;
  }
  if (local_slowdown == 0.0) {
    char *env = getenv("VALGRIND_SLOWDOWN");
    slowdown = local_slowdown = env ? atof(env) : 50.0;
  }
  return local_slowdown;
}

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  /* DYNAMIC_ANNOTATIONS_EXTERNAL_IMPL == 0 */
