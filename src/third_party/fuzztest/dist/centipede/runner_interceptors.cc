// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Function interceptors for Centipede.

#include <dlfcn.h>  // for dlsym()
#include <pthread.h>

#include <cstdint>
#include <cstring>

#include "absl/base/nullability.h"
#include "absl/base/optimization.h"
#include "./centipede/runner.h"

using fuzztest::internal::tls;

// Used for the interceptors to avoid sanitizing them, as they could be called
// before or during the sanitizer initialization. Instead, we check if the
// current thread is marked as started by the runner as the proxy of sanitizier
// initialization. If not, we skip the interception logic.
#define NO_SANITIZE __attribute__((no_sanitize("all")))

namespace {

// Wrapper for dlsym().
// Returns the pointer to the real function `function_name`.
// In most cases we need FuncAddr("foo") to be called before the first call to
// foo(), which means we either need to do this very early at startup
// (e.g. pre-init array), or on the first call.
// Currently, we do this on the first call via function-scope static.
template <typename FunctionT>
FunctionT FuncAddr(const char *function_name) {
  void *addr = dlsym(RTLD_NEXT, function_name);
  return reinterpret_cast<FunctionT>(addr);
}

// 3rd and 4th arguments to pthread_create(), packed into a struct.
struct ThreadCreateArgs {
  void *(*start_routine)(void *);
  void *arg;
};

// Wrapper for a `start_routine` argument of pthread_create().
// Calls the actual start_routine and returns its results.
// Performs custom actions before and after start_routine().
// `arg` is a `ThreadCreateArgs *` with the actual pthread_create() args.
void *MyThreadStart(void *absl_nonnull arg) {
  auto *args_orig_ptr = static_cast<ThreadCreateArgs *>(arg);
  auto args = *args_orig_ptr;
  delete args_orig_ptr;  // allocated in the pthread_create wrapper.
  tls.OnThreadStart();
  void *retval = args.start_routine(args.arg);
  return retval;
}

// Normalize the *cmp result value to be one of {1, -1, 0}.
// According to the spec, *cmp can return any positive or negative value,
// and in fact it does return various different positive and negative values
// depending on <some random factors>. These values are later passed to our
// CMP instrumentation and are used to produce features.
// If we don't normalize the return value here, our tests may be flaky.
int NormalizeCmpResult(int result) {
  if (result < 0) return -1;
  if (result > 0) return 1;
  return result;
}

}  // namespace

namespace fuzztest::internal {
void RunnerInterceptor() {}  // to be referenced in runner.cc
}  // namespace fuzztest::internal

// A sanitizer-compatible way to intercept functions that are potentially
// intercepted by sanitizers, in which case the symbol __interceptor_X would be
// defined for intercepted function X. So we always forward an intercepted call
// to the sanitizer interceptor if it exists, and fall back to the next
// definition following dlsym.
//
// We define the X_orig pointers that are statically initialized to GetOrig_X()
// with the aforementioned logic to fill the pointers early, but they might
// still be too late. So the Centipede interceptors might need to handle the
// nullptr case and/or use REAL(X), which calls GetOrig_X() when needed. Also
// see compiler-rt/lib/interception/interception.h in the llvm-project source
// code.
//
// Note that since LLVM 17 it allows three interceptions (from the original
// binary, an external tool, and a sanitizer) to co-exist under a new scheme,
// while it is still compatible with the old way used here.
#define SANITIZER_INTERCEPTOR_NAME(orig_func_name) \
  __interceptor_##orig_func_name
#define DECLARE_CENTIPEDE_ORIG_FUNC(ret_type, orig_func_name, args)         \
  extern "C" __attribute__((weak)) ret_type(                                \
      SANITIZER_INTERCEPTOR_NAME(orig_func_name)) args;                     \
  static decltype(&SANITIZER_INTERCEPTOR_NAME(                              \
      orig_func_name)) GetOrig_##orig_func_name() {                         \
    if (auto p = &SANITIZER_INTERCEPTOR_NAME(orig_func_name)) return p;     \
    return FuncAddr<decltype(&SANITIZER_INTERCEPTOR_NAME(orig_func_name))>( \
        #orig_func_name);                                                   \
  }                                                                         \
  static ret_type(*orig_func_name##_orig) args;                             \
  __attribute__((constructor)) void InitializeOrig_##orig_func_name() {     \
    orig_func_name##_orig = GetOrig_##orig_func_name();                     \
  }
#define REAL(orig_func_name) \
  (orig_func_name##_orig ? orig_func_name##_orig : GetOrig_##orig_func_name())

DECLARE_CENTIPEDE_ORIG_FUNC(int, memcmp,
                            (const void *s1, const void *s2, size_t n));
DECLARE_CENTIPEDE_ORIG_FUNC(int, strcmp, (const char *s1, const char *s2));
DECLARE_CENTIPEDE_ORIG_FUNC(int, strncmp,
                            (const char *s1, const char *s2, size_t n));
DECLARE_CENTIPEDE_ORIG_FUNC(int, pthread_create,
                            (pthread_t * thread, const pthread_attr_t *attr,
                             void *(*start_routine)(void *), void *arg));

// Fallback for the case *cmp_orig is null.
// Will be executed several times at process startup, if at all.
static NO_SANITIZE int memcmp_fallback(const void *s1, const void *s2,
                                       size_t n) {
  const auto *p1 = static_cast<const uint8_t *>(s1);
  const auto *p2 = static_cast<const uint8_t *>(s2);
  for (size_t i = 0; i < n; ++i) {
    int diff = p1[i] - p2[i];
    if (diff) return diff;
  }
  return 0;
}

// memcmp interceptor.
// Calls the real memcmp() and possibly modifies state.cmp_feature_set.
extern "C" NO_SANITIZE int memcmp(const void *s1, const void *s2, size_t n) {
  const int result =
      memcmp_orig ? memcmp_orig(s1, s2, n) : memcmp_fallback(s1, s2, n);
  if (ABSL_PREDICT_FALSE(!tls.started)) {
    return result;
  }
  tls.TraceMemCmp(reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                  reinterpret_cast<const uint8_t *>(s1),
                  reinterpret_cast<const uint8_t *>(s2), n, result == 0);
  return NormalizeCmpResult(result);
}

// TODO(b/341111359): Investigate inefficiencies in the `strcmp`/`strncmp`
// interceptors and `TraceMemCmp`.

// strcmp interceptor.
// Calls the real strcmp() and possibly modifies state.cmp_feature_set.
extern "C" NO_SANITIZE int strcmp(const char *s1, const char *s2) {
  // Find the length of the shorter string, as this determines the actual number
  // of bytes that are compared. Note that this is needed even if we call
  // `strcmp_orig` because we're passing it to `TraceMemCmp()`.
  size_t len = 0;
  while (s1[len] && s2[len]) ++len;
  const int result =
      // Need to include one more byte than the shorter string length
      // when falling back to memcmp e.g. "foo" < "foobar".
      strcmp_orig ? strcmp_orig(s1, s2) : memcmp_fallback(s1, s2, len + 1);
  if (ABSL_PREDICT_FALSE(!tls.started)) {
    return result;
  }
  // Pass `len` here to avoid storing the trailing '\0' in the dictionary.
  tls.TraceMemCmp(reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                  reinterpret_cast<const uint8_t *>(s1),
                  reinterpret_cast<const uint8_t *>(s2), len, result == 0);
  return NormalizeCmpResult(result);
}

// strncmp interceptor.
// Calls the real strncmp() and possibly modifies state.cmp_feature_set.
extern "C" NO_SANITIZE int strncmp(const char *s1, const char *s2, size_t n) {
  // Find the length of the shorter string, as this determines the actual number
  // of bytes that are compared. Note that this is needed even if we call
  // `strncmp_orig` because we're passing it to `TraceMemCmp()`.
  size_t len = 0;
  while (len < n && s1[len] && s2[len]) ++len;
  // Need to include '\0' in the comparison if the shorter string is shorter
  // than `n`, hence we add 1 to the length.
  if (n > len + 1) n = len + 1;
  const int result =
      strncmp_orig ? strncmp_orig(s1, s2, n) : memcmp_fallback(s1, s2, n);
  if (ABSL_PREDICT_FALSE(!tls.started)) {
    return result;
  }
  // Pass `len` here to avoid storing the trailing '\0' in the dictionary.
  tls.TraceMemCmp(reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                  reinterpret_cast<const uint8_t *>(s1),
                  reinterpret_cast<const uint8_t *>(s2), len, result == 0);
  return NormalizeCmpResult(result);
}

// pthread_create interceptor.
// Calls real pthread_create, but wraps the start_routine() in MyThreadStart.
extern "C" int pthread_create(pthread_t *absl_nonnull thread,
                              const pthread_attr_t *absl_nullable attr,
                              void *(*start_routine)(void *),
                              void *absl_nullable arg) {
  if (ABSL_PREDICT_FALSE(!tls.started)) {
    return REAL(pthread_create)(thread, attr, start_routine, arg);
  }
  // Wrap the arguments. Will be deleted in MyThreadStart.
  auto *wrapped_args = new ThreadCreateArgs{start_routine, arg};
  // Run the actual pthread_create.
  return REAL(pthread_create)(thread, attr, MyThreadStart, wrapped_args);
}
